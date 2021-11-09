#include "agent_config.h"
#include "toml.h"
#include <algorithm>
#include <stdlib.h>
#include "ppconsul/kv.h"
#include <iostream>   // std::cout
#include <string>     // std::string, std::stod
#include <chrono>


using ppconsul::Consul;
using ppconsul::Consistency;
using namespace ppconsul::kv;
using namespace std::chrono;


long last_updated_time = 0;

struct ScopedTable {
  ScopedTable(toml_table_t* table) : table(table) {}
  ~ScopedTable() { toml_free(table); }

  toml_table_t* table;
};

static long double curtime() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  ).count();
}

static std::string FromStringDatum(toml_datum_t datum) {
  std::string val{datum.u.s};
  free(datum.u.s);
  return val;
}

static bool SetupOtlpExporter(toml_table_t* table, ngx_log_t* log, OtelNgxAgentConfig* config) {
  const char *otel_exporter_otlp_endpoint_env = "OTEL_EXPORTER_OTLP_ENDPOINT";
  auto endpoint_from_env = std::getenv(otel_exporter_otlp_endpoint_env);

  if (endpoint_from_env) {
    config->exporter.endpoint = endpoint_from_env;
    return true;
  }

  toml_datum_t hostVal = toml_string_in(table, "host");
  toml_datum_t portVal = toml_int_in(table, "port");

  if (!hostVal.ok) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Missing required host field for OTLP exporter");
    return false;
  }

  std::string host = FromStringDatum(hostVal);

  if (!portVal.ok) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Missing required port field for OTLP exporter");
    return false;
  }

  config->exporter.endpoint = host + ":" + std::to_string(portVal.u.i);;

  return true;
}

static bool SetupExporter(toml_table_t* root, ngx_log_t* log, OtelNgxAgentConfig* config) {
  toml_datum_t exporterVal = toml_string_in(root, "exporter");

  if (!exporterVal.ok) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Missing required exporter field");
    return false;
  }

  std::string exporter = FromStringDatum(exporterVal);

  toml_table_t* exporters = toml_table_in(root, "exporters");

  if (!exporters) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Unable to find exporters table");
    return false;
  }

  if (exporter == "otlp") {
    toml_table_t* otlp = toml_table_in(exporters, "otlp");

    if (!otlp) {
      ngx_log_error(NGX_LOG_ERR, log, 0, "Unable to find exporters.otlp");
      return false;
    }

    if (!SetupOtlpExporter(otlp, log, config)) {
      return false;
    }

    config->exporter.type = OtelExporterOTLP;
  } else {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Unsupported exporter %s", exporter.c_str());
    return false;
  }

  return true;
}

static bool SetupService(toml_table_t* root, ngx_log_t*, OtelNgxAgentConfig* config) {
  toml_table_t* service = toml_table_in(root, "service");

  if (service) {
    toml_datum_t serviceName = toml_string_in(service, "name");

    if (serviceName.ok) {
      config->service.name = FromStringDatum(serviceName);
    }
  }

  return true;
}

static bool SetupProcessor(toml_table_t* root, ngx_log_t* log, OtelNgxAgentConfig* config) {
  toml_datum_t processorVal = toml_string_in(root, "processor");

  if (!processorVal.ok) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Unable to find required processor field");
    return false;
  }

  std::string processor = FromStringDatum(processorVal);

  if (processor != "batch") {
    config->processor.type = OtelProcessorSimple;
    return true;
  }

  config->processor.type = OtelProcessorBatch;

  toml_table_t* processors = toml_table_in(root, "processors");

  if (!processors) {
    // Go with the default batch processor config
    return true;
  }

  toml_table_t* batchProcessor = toml_table_in(processors, "batch");

  if (!batchProcessor) {
    return true;
  }

  toml_datum_t maxQueueSize = toml_int_in(batchProcessor, "max_queue_size");

  if (maxQueueSize.ok) {
    config->processor.batch.maxQueueSize = std::max(int64_t(1), maxQueueSize.u.i);
  }

  toml_datum_t scheduleDelayMillis = toml_int_in(batchProcessor, "schedule_delay_millis");

  if (scheduleDelayMillis.ok) {
    config->processor.batch.scheduleDelayMillis = std::max(int64_t(0), scheduleDelayMillis.u.i);
  }

  toml_datum_t maxExportBatchSize = toml_int_in(batchProcessor, "max_export_batch_size");

  if (maxExportBatchSize.ok) {
    config->processor.batch.maxExportBatchSize = std::max(int64_t(1), maxExportBatchSize.u.i);
  }

  return true;
}

static double getSamplingRate(std::string cmdb, std::string cur_env, ngx_log_t* log){
    long cur = curtime();
    std::cout<<std::to_string(cur)<<" cur.\n";
    std::cout<<std::to_string(last_updated_time)<<" last_updated_time.\n";
    if((cur - last_updated_time) > 1000 * 60 * 3){
      last_updated_time = cur;
      std::cout<<" getSamplingRate.\n";
      std::string cur_token = cur_env=="prod"?"4e13740e-9d65-39eb-e0c3-473397658ea6":"eb438d90-4183-06d7-0095-8e24d723c9c6";
      std::string cur_url = cur_env == "prod"?"http://internal-ms-service-discovery-887102973.ap-northeast-2.elb.amazonaws.com/":"http://internal-ms-consul-alb-416054892.ap-northeast-2.elb.amazonaws.com:8500";
      ppconsul::Consul consul(cur_url,kw::token=cur_token);
      Kv kv(consul,kw::token=cur_token);
      return stod(kv.get("hot_config/coutrace/nginx/" + cmdb , "1", kw::token=cur_token));
    }
    return -1.0;
    //return 1.0;
}

static bool SetupSampler(toml_table_t* root, ngx_log_t* log, OtelNgxAgentConfig* config) {
  toml_table_t* sampler = toml_table_in(root, "sampler");

  if (!sampler) {
    return true;
  }
  toml_datum_t toml_cmdb = toml_string_in(sampler, "cmdb");
  toml_datum_t toml_env = toml_string_in(sampler, "env");
  std::string cmdb;
  std::string cur_env;
  
  if(!toml_cmdb.ok){
    cmdb = "default";
  } else {
    cmdb = FromStringDatum(toml_cmdb);
  }

  if(!toml_env.ok){
    cur_env = "dev";
  } else {
    cur_env = FromStringDatum(toml_env);
  }
  toml_datum_t samplerNameVal = toml_string_in(sampler, "name");
  if (samplerNameVal.ok) {
    std::string samplerName = FromStringDatum(samplerNameVal);

    if (samplerName == "AlwaysOn") {
      config->sampler.type = OtelSamplerAlwaysOn;
    } else if (samplerName == "AlwaysOff") {
      config->sampler.type = OtelSamplerAlwaysOff;
    } else if (samplerName == "TraceIdRatioBased") {
      config->sampler.type = OtelSamplerTraceIdRatioBased;

      toml_datum_t ratio = toml_double_in(sampler, "ratio");

      if (ratio.ok) {
        //config->sampler.ratio = ratio.u.d;
        double ratio = getSamplingRate(cmdb, cur_env, log);
        if(ratio != -1.0){
          long cur = curtime();
          config->sampler.ratio = ratio;
        }
        config->sampler.cmdb = cmdb;
        config->sampler.env = cur_env;
      } else {
        ngx_log_error(NGX_LOG_ERR, log, 0, "TraceIdRatioBased requires a ratio to be specified");
        return false;
      }
    } else {
      ngx_log_error(NGX_LOG_ERR, log, 0, "Unsupported sampler %s", samplerName.c_str());
      return false;
    }
  }

  toml_datum_t parentBased = toml_bool_in(sampler, "parent_based");

  if (parentBased.ok) {
    config->sampler.parentBased = parentBased.u.b;
  }

  return true;
}

bool OtelAgentConfigLoad(const std::string& path, ngx_log_t* log, OtelNgxAgentConfig* config) {
  FILE* confFile = fopen(path.c_str(), "r");

  if (!confFile) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Unable to open agent config file at %s", path.c_str());
    return false;
  }

  char errBuf[256] = {0};
  ScopedTable scopedConf{toml_parse_file(confFile, errBuf, sizeof(errBuf))};
  fclose(confFile);

  if (!scopedConf.table) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "Configuration error: %s", errBuf);
    return false;
  }

  toml_table_t* root = scopedConf.table;

  if (!SetupExporter(root, log, config)) {
    return false;
  }

  if (!SetupService(root, log, config)) {
    return false;
  }

  if (!SetupProcessor(root, log, config)) {
    return false;
  }

  if (!SetupSampler(root, log, config)) {
    return false;
  }

  return true;
}
