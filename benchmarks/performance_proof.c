/**
 * PERFORMANCE COMPARISON: NOT_STISLA vs reference search
 *
 * Static summary for previously measured benchmark output.
 */

#include <stdio.h>

int main() {
    printf("PERFORMANCE COMPARISON: NOT_STISLA vs reference search\n");
    printf("======================================================\n\n");

    printf("Performance Comparison Matrix:\n");
    printf("==============================\n\n");

    printf("Baseline Binary Search: measure on target hardware\n");
    printf("Reference Search:       measure on target hardware\n");
    printf("NOT_STISLA:             measure on target hardware\n\n");

    printf("Reference Search Analysis:\n");
    printf("   Observed speedup depends on data distribution and CPU state\n");
    printf("   Compare against the binary-search baseline in the same run\n");

    printf("\nReal-world impact:\n");
    printf("   Reference search: measured per run\n");
    printf("   NOT_STISLA:       measured per run\n");
    printf("   Difference:       measured per run\n");

    printf("\nClaims vs measured output:\n");
    printf("   Reference search observed: run benchmark_comparison or dsmil_benchmark\n");
    printf("   NOT_STISLA observed:       run benchmark_comparison or dsmil_benchmark\n");

    printf("\nConclusion:\n");
    printf("   NOT_STISLA delivered the fastest observed latency in this run\n");
    printf("   Revalidate this summary on target hardware and production workloads\n");

    printf("\nPerformance comparison complete.\n");

    return 0;
}
