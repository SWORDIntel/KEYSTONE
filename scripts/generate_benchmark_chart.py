#!/usr/bin/env python3
"""
Generate benchmark_comparison.png from compare_search_auto CSV output.
Supports multi-profile runs (different array sizes) for scaling analysis.
"""

import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

def parse_csv(path):
    headers = None
    profiles = []
    cur_rows = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith('#profile='):
                if cur_rows:
                    profiles.append((profile_name, headers, cur_rows))
                    cur_rows = []
                profile_name = line.split('=', 1)[1]
            elif line.startswith('#run,'):
                headers = line[1:].split(',')
            elif not line.startswith('#'):
                cur_rows.append(line.split(','))
        if cur_rows:
            profiles.append((profile_name, headers, cur_rows))
    return profiles

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <csv_file> [output_png]", file=sys.stderr)
        sys.exit(1)

    csv_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'benchmark_comparison.png'

    profiles = parse_csv(csv_path)
    if not profiles:
        print("No data found in CSV", file=sys.stderr)
        sys.exit(1)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))

    labels = ['Binary', 'StiSorter', 'Enhanced', 'Parallel', 'Fortran', 'Auto']
    keys = ['binary_ns', 'not_stisla_ns', 'enhanced_ns', 'batch_parallel_ns', 'fortran_batch_ns', 'auto_batch_ns']
    colors = ['#2ca02c', '#ff7f0e', '#1f77b4', '#9467bd', '#d62728', '#8c564b']

    # Aggregate across all profiles into single averages
    avgs = {}
    for h in keys:
        vals = []
        for _, headers, rows in profiles:
            col = headers.index(h)
            vals += [float(r[col]) for r in rows]
        avgs[h] = np.mean(vals)

    binary_avg = avgs['binary_ns']
    speedups = {}
    for k in keys:
        if k != 'binary_ns' and avgs[k] > 0:
            speedups[k] = binary_avg / avgs[k]

    x = np.arange(len(labels))
    width = 0.5
    bar_vals = [avgs[k] for k in keys]
    bars1 = ax1.bar(x, bar_vals, width, color=colors, alpha=0.8, edgecolor='black')
    ax1.set_ylabel('Latency (ns/op)')
    ax1.set_title('Average Search Latency by Backend')
    ax1.set_xticks(x)
    ax1.set_xticklabels(labels, rotation=15, ha='right')
    ax1.set_yscale('log')
    ax1.grid(True, alpha=0.3, axis='y')
    for bar in bars1:
        height = bar.get_height()
        ax1.annotate(f'{height:.1f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3), textcoords="offset points",
                    ha='center', va='bottom', fontsize=8)

    speed_labels = ['StiSorter', 'Enhanced', 'Parallel', 'Fortran', 'Auto']
    speed_keys = ['not_stisla_ns', 'enhanced_ns', 'batch_parallel_ns', 'fortran_batch_ns', 'auto_batch_ns']
    x2 = np.arange(len(speed_labels))
    speed_vals = [speedups.get(k, 0) for k in speed_keys]
    bars2 = ax2.bar(x2, speed_vals, width, color=colors[1:], alpha=0.8, edgecolor='black')
    ax2.axhline(y=1.0, color='gray', linestyle='--', linewidth=1)
    ax2.set_ylabel('Speedup vs Binary')
    ax2.set_title('Average Speedup Factor (higher is better)')
    ax2.set_xticks(x2)
    ax2.set_xticklabels(speed_labels, rotation=15, ha='right')
    ax2.grid(True, alpha=0.3, axis='y')
    for bar in bars2:
        height = bar.get_height()
        ax2.annotate(f'{height:.1f}x',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3), textcoords="offset points",
                    ha='center', va='bottom', fontsize=8)

    plt.suptitle('StiSorter Benchmark Comparison', fontsize=14, fontweight='bold')
    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight')
    print(f"Saved benchmark chart to: {out_path}")

if __name__ == '__main__':
    main()
