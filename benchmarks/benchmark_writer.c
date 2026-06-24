#include "benchmark_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/resource.h>

struct keystone_bench_writer {
    FILE* f;
    keystone_bench_format_t format;
    int is_stdout;
    int needs_header;
};

keystone_bench_writer_t* keystone_bench_writer_create(const keystone_bench_writer_config_t* config) {
    if (!config) return NULL;

    keystone_bench_writer_t* writer = calloc(1, sizeof(keystone_bench_writer_t));
    if (!writer) return NULL;

    writer->format = config->format;
    writer->is_stdout = (config->file_path == NULL);
    
    if (writer->is_stdout) {
        writer->f = stdout;
    } else {
        writer->f = fopen(config->file_path, config->append ? "a" : "w");
        if (!writer->f) {
            free(writer);
            return NULL;
        }
    }

    writer->needs_header = (config->append == 0 || writer->is_stdout);

    if (writer->format == KEYSTONE_BENCH_FORMAT_CSV && writer->needs_header) {
        fprintf(writer->f, "host,compiler,cpu_features,profile,n,queries,hit_rate,gap,jitter,stride,threads,backend,source,shape,calibration_runs,candidates,throughput_gib_s,median_ns,p95_ns,rss_kb,page_faults\n");
    } else if (writer->format == KEYSTONE_BENCH_FORMAT_JSON && writer->needs_header) {
        fprintf(writer->f, "[\n");
        writer->needs_header = 0; /* use as 'is_first_item' flag for JSON */
    }

    return writer;
}

void keystone_bench_writer_destroy(keystone_bench_writer_t* writer) {
    if (writer) {
        if (writer->format == KEYSTONE_BENCH_FORMAT_JSON) {
            fprintf(writer->f, "\n]\n");
        }
        if (!writer->is_stdout && writer->f) {
            fclose(writer->f);
        }
        free(writer);
    }
}

int keystone_bench_writer_record(keystone_bench_writer_t* writer, const keystone_bench_record_t* record) {
    if (!writer || !record) return -1;

    struct utsname name;
    if (uname(&name) < 0) {
        strcpy(name.nodename, "unknown");
    }

    const char* compiler = "unknown";
#if defined(__clang__)
    compiler = "clang";
#elif defined(__GNUC__)
    compiler = "gcc";
#endif

    struct rusage usage;
    long rss_kb = 0;
    long page_faults = 0;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        rss_kb = usage.ru_maxrss;
        page_faults = usage.ru_majflt;
    }

    if (writer->format == KEYSTONE_BENCH_FORMAT_CSV) {
        fprintf(writer->f, "%s,%s,0x%08x,%s,%zu,%zu,%d,%d,%d,%d,%d,%s,%s,%s,%zu,%zu,%.3f,%.2f,%.2f,%ld,%ld\n",
                name.nodename, compiler, record->cpu_features, record->profile_name ? record->profile_name : "none",
                record->array_size, record->num_queries, record->hit_rate_pct, record->data_gap,
                record->data_gap_jitter, record->query_stride, record->thread_count,
                record->decision_available ? record->backend_name : "none",
                record->decision_available ? record->decision_source : "none",
                record->decision_available ? record->query_shape : "none",
                record->calibration_runs, record->candidates_measured,
                record->throughput_gib_s, record->median_latency_ns, record->p95_latency_ns,
                rss_kb, page_faults);
    } else {
        if (!writer->needs_header) {
            fprintf(writer->f, ",\n");
        }
        writer->needs_header = 0;
        
        fprintf(writer->f, "  {\n");
        fprintf(writer->f, "    \"host\": \"%s\",\n", name.nodename);
        fprintf(writer->f, "    \"compiler\": \"%s\",\n", compiler);
        fprintf(writer->f, "    \"cpu_features\": %u,\n", record->cpu_features);
        fprintf(writer->f, "    \"profile\": \"%s\",\n", record->profile_name ? record->profile_name : "none");
        fprintf(writer->f, "    \"n\": %zu,\n", record->array_size);
        fprintf(writer->f, "    \"queries\": %zu,\n", record->num_queries);
        fprintf(writer->f, "    \"hit_rate\": %d,\n", record->hit_rate_pct);
        fprintf(writer->f, "    \"gap\": %d,\n", record->data_gap);
        fprintf(writer->f, "    \"jitter\": %d,\n", record->data_gap_jitter);
        fprintf(writer->f, "    \"stride\": %d,\n", record->query_stride);
        fprintf(writer->f, "    \"threads\": %d,\n", record->thread_count);
        fprintf(writer->f, "    \"backend\": \"%s\",\n", record->decision_available ? record->backend_name : "none");
        fprintf(writer->f, "    \"source\": \"%s\",\n", record->decision_available ? record->decision_source : "none");
        fprintf(writer->f, "    \"shape\": \"%s\",\n", record->decision_available ? record->query_shape : "none");
        fprintf(writer->f, "    \"calibration_runs\": %zu,\n", record->calibration_runs);
        fprintf(writer->f, "    \"candidates\": %zu,\n", record->candidates_measured);
        fprintf(writer->f, "    \"throughput_gib_s\": %.3f,\n", record->throughput_gib_s);
        fprintf(writer->f, "    \"median_ns\": %.2f,\n", record->median_latency_ns);
        fprintf(writer->f, "    \"p95_ns\": %.2f,\n", record->p95_latency_ns);
        fprintf(writer->f, "    \"rss_kb\": %ld,\n", rss_kb);
        fprintf(writer->f, "    \"page_faults\": %ld\n", page_faults);
        fprintf(writer->f, "  }");
    }

    fflush(writer->f);
    return 0;
}
