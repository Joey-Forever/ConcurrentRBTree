#!/usr/bin/env python3
"""
Plot Write Probability Throughput Comparison between RBTree and ConcurrentSkipList

Usage: python3 plot_throughput.py
"""

import matplotlib.pyplot as plt
import numpy as np
import sys

def parse_write_prob_output(filename):
    """Parse the write probability output file"""
    with open(filename, 'r') as f:
        lines = f.readlines()

    data = {
        'write_prob': [],
        'read_throughput': [],
        'insert_throughput': [],
        'erase_throughput': [],
        'read_latency': [],
        'insert_latency': [],
        'erase_latency': [],
        'thread_cnt': None,
        'init_data': None
    }

    for line in lines:
        # Extract thread count
        if 'total threads:' in line:
            try:
                data['thread_cnt'] = int(line.split('total threads:')[1].split()[0].strip())
            except (ValueError, IndexError):
                pass

        # Extract init data size
        if 'init data size:' in line:
            try:
                data['init_data'] = int(line.split('init data size:')[1].split()[0].strip())
            except (ValueError, IndexError):
                pass

    in_data_section = False
    for line in lines:
        if 'Write Prob' in line and 'Throughput' in line:
            in_data_section = True
            continue
        if '--------' in line and in_data_section:
            continue
        if in_data_section:
            parts = line.split('|')
            if len(parts) >= 7:
                try:
                    write_prob = float(parts[0].strip())
                    read_tp = float(parts[1].strip().replace(',', ''))
                    insert_tp = float(parts[2].strip().replace(',', ''))
                    erase_tp = float(parts[3].strip().replace(',', ''))
                    read_lat = float(parts[4].strip().replace(',', ''))
                    insert_lat = float(parts[5].strip().replace(',', ''))
                    erase_lat = float(parts[6].strip().replace(',', ''))

                    data['write_prob'].append(write_prob)
                    data['read_throughput'].append(read_tp)
                    data['insert_throughput'].append(insert_tp)
                    data['erase_throughput'].append(erase_tp)
                    data['read_latency'].append(read_lat)
                    data['insert_latency'].append(insert_lat)
                    data['erase_latency'].append(erase_lat)
                except (ValueError, IndexError):
                    continue

    return data

def main():
    # Fixed file names
    rbtree_file = 'rbtree_result.txt'
    skiplist_file = 'skip_list_result.txt'

    # Parse both output files
    rbtree_data = parse_write_prob_output(rbtree_file)
    skiplist_data = parse_write_prob_output(skiplist_file)

    # Validate and get thread count and init data size
    if rbtree_data['thread_cnt'] is None or rbtree_data['init_data'] is None:
        print(f"Error: Cannot extract thread count or init data size from {rbtree_file}")
        sys.exit(1)

    thread_count = rbtree_data['thread_cnt']
    init_data_size = rbtree_data['init_data']

    # Check if both files have the same thread count and init data size
    if skiplist_data['thread_cnt'] != thread_count:
        print(f"Error: Thread count mismatch between files:")
        print(f"  {rbtree_file}: {rbtree_data['thread_cnt']}")
        print(f"  {skiplist_file}: {skiplist_data['thread_cnt']}")
        sys.exit(1)

    if skiplist_data['init_data'] != init_data_size:
        print(f"Error: Init data size mismatch between files:")
        print(f"  {rbtree_file}: {rbtree_data['init_data']}")
        print(f"  {skiplist_file}: {skiplist_data['init_data']}")
        sys.exit(1)

    output_file = f'throughput_{thread_count}threads_{init_data_size}init.png'

    write_probs = rbtree_data['write_prob']

    # Create figure with 3 subplots (1 row, 3 columns)
    fig, axes = plt.subplots(1, 3, figsize=(18, 5))
    fig.suptitle(f'Throughput Comparison: RBTree vs ConcurrentSkipList\n'
                 f'Threads: {thread_count}, Init Data Size: {init_data_size}, 50%% insert, 50%% erase',
                 fontsize=16, fontweight='bold')

    # Calculate average ratios for throughput (RBTree / SkipList)
    avg_read_tp_ratio = np.mean([rbtree_data['read_throughput'][i] / skiplist_data['read_throughput'][i]
                                 for i in range(len(write_probs))])
    avg_insert_tp_ratio = np.mean([rbtree_data['insert_throughput'][i] / skiplist_data['insert_throughput'][i]
                                   for i in range(len(write_probs))])
    avg_erase_tp_ratio = np.mean([rbtree_data['erase_throughput'][i] / skiplist_data['erase_throughput'][i]
                                  for i in range(len(write_probs))])

    # Plot 1: Read Throughput vs Write Probability
    ax = axes[0]
    ax.plot(write_probs, rbtree_data['read_throughput'], 'o-', label='RBTree', linewidth=2, markersize=6, color='blue')
    ax.plot(write_probs, skiplist_data['read_throughput'], 's-', label='SkipList', linewidth=2, markersize=6, color='green')
    ax.set_xlabel('Write Probability', fontsize=12)
    ax.set_ylabel('Read Throughput (ops/s)', fontsize=12)
    ax.set_title(f'Read Throughput (RBTree {avg_read_tp_ratio:.2f}x SkipList)', fontsize=13, fontweight='bold')
    ax.set_xscale('log')
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=11)

    # Plot 2: Insert Throughput vs Write Probability
    ax = axes[1]
    ax.plot(write_probs, rbtree_data['insert_throughput'], 'o-', label='RBTree', linewidth=2, markersize=6, color='red')
    ax.plot(write_probs, skiplist_data['insert_throughput'], 's-', label='SkipList', linewidth=2, markersize=6, color='orange')
    ax.set_xlabel('Write Probability', fontsize=12)
    ax.set_ylabel('Insert Throughput (ops/s)', fontsize=12)
    ax.set_title(f'Insert Throughput (RBTree {avg_insert_tp_ratio:.2f}x SkipList)', fontsize=13, fontweight='bold')
    ax.set_xscale('log')
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=11)

    # Plot 3: Erase Throughput vs Write Probability
    ax = axes[2]
    ax.plot(write_probs, rbtree_data['erase_throughput'], 'o-', label='RBTree', linewidth=2, markersize=6, color='darkred')
    ax.plot(write_probs, skiplist_data['erase_throughput'], 's-', label='SkipList', linewidth=2, markersize=6, color='darkorange')
    ax.set_xlabel('Write Probability', fontsize=12)
    ax.set_ylabel('Erase Throughput (ops/s)', fontsize=12)
    ax.set_title(f'Erase Throughput (RBTree {avg_erase_tp_ratio:.2f}x SkipList)', fontsize=13, fontweight='bold')
    ax.set_xscale('log')
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=11)

    plt.tight_layout(rect=[0, 0.03, 1, 0.93])
    plt.savefig(output_file, dpi=150)
    print(f"Chart saved as {output_file}")

    # Print summary statistics
    print("\n" + "="*130)
    print(f"THROUGHPUT PERFORMANCE COMPARISON: RBTree vs ConcurrentSkipList")
    print(f"Threads: {thread_count}, Init Data Size: {init_data_size}, 50%% insert, 50%% erase, averaged over 5 runs")
    print("="*130)
    print(f"{'Write Prob':<12} {'RB Read':<12} {'SL Read':<12} {'RB Ins':<12} {'SL Ins':<12} {'RB Era':<12} {'SL Era':<12}")
    print(f"{'':12} {'(ops/s)':<12} {'(ops/s)':<12} {'(ops/s)':<12} {'(ops/s)':<12} {'(ops/s)':<12} {'(ops/s)':<12}")
    print("-"*130)
    for i in range(len(write_probs)):
        print(f"{write_probs[i]:<12.6f} "
              f"{rbtree_data['read_throughput'][i]:<12.0f} {skiplist_data['read_throughput'][i]:<12.0f} "
              f"{rbtree_data['insert_throughput'][i]:<12.0f} {skiplist_data['insert_throughput'][i]:<12.0f} "
              f"{rbtree_data['erase_throughput'][i]:<12.0f} {skiplist_data['erase_throughput'][i]:<12.0f}")

    # Calculate throughput ratios
    print("\n" + "="*80)
    print("THROUGHPUT RATIO (SkipList / RBTree, <1 means SkipList is better)")
    print("="*80)
    print(f"{'Write Prob':<12} {'Read Ratio':<12} {'Insert Ratio':<12} {'Erase Ratio':<12}")
    print("-"*50)
    for i in range(len(write_probs)):
        read_ratio = skiplist_data['read_throughput'][i] / rbtree_data['read_throughput'][i]
        insert_ratio = skiplist_data['insert_throughput'][i] / rbtree_data['insert_throughput'][i]
        erase_ratio = skiplist_data['erase_throughput'][i] / rbtree_data['erase_throughput'][i]
        print(f"{write_probs[i]:<12.6f} {read_ratio:<12.2f} {insert_ratio:<12.2f} {erase_ratio:<12.2f}")

    avg_read_ratio = np.mean([skiplist_data['read_throughput'][i] / rbtree_data['read_throughput'][i]
                              for i in range(len(write_probs))])
    avg_insert_ratio = np.mean([skiplist_data['insert_throughput'][i] / rbtree_data['insert_throughput'][i]
                                 for i in range(len(write_probs))])
    avg_erase_ratio = np.mean([skiplist_data['erase_throughput'][i] / rbtree_data['erase_throughput'][i]
                                for i in range(len(write_probs))])

    print("-"*50)
    print(f"{'Average':<12} {avg_read_ratio:<12.2f} {avg_insert_ratio:<12.2f} {avg_erase_ratio:<12.2f}")

    plt.show()

if __name__ == "__main__":
    main()
