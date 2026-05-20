#!/usr/bin/env python3
"""
Generate 4-panel BLER plots for PC5 Sidelink
Supports both Remote UE RX (nearby) and Syncref RX perspectives
"""

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import sys
from pathlib import Path
from matplotlib.patches import Ellipse
from scipy.interpolate import make_interp_spline

def plot_bler_4panel(results_dir, perspective='nearby'):
    """
    Generate 4-panel BLER plot

    Args:
        results_dir: Directory containing CSV files
        perspective: 'nearby' (Remote UE RX) or 'syncref_rx' (Syncref RX)
    """
    results_dir = Path(results_dir).expanduser()
    print(f"Loading data from: {results_dir}")

    # Determine file names and titles based on perspective
    if perspective == 'nearby':
        bler_csv = results_dir / "nearby_bler_combined.csv"
        ldpc_csv = results_dir / "nearby_ldpc_combined.csv"
        output_file = results_dir / "nearby_bler_4panel.png"
        main_title = 'Performance Analysis of 5G SL Mode 1 (Remote UE Rx)'
        subplot_title = 'PC5 MAC BLER vs SNR (Remote UE RX)'
    elif perspective == 'syncref_rx':
        bler_csv = results_dir / "syncref_rx_bler_combined.csv"
        ldpc_csv = results_dir / "syncref_rx_ldpc_combined.csv"
        output_file = results_dir / "syncref_rx_bler_4panel.png"
        main_title = 'Performance Analysis of 5G SL Mode 1 (Syncref Rx)'
        subplot_title = 'PC5 MAC BLER vs SNR (Syncref RX from Nearby)'
    else:
        print(f"Error: Unknown perspective '{perspective}'. Use 'nearby' or 'syncref_rx'")
        sys.exit(1)

    # Load BLER data
    if not bler_csv.exists():
        print(f"Error: {bler_csv} not found")
        sys.exit(1)

    df = pd.read_csv(bler_csv)
    print(f"Loaded {len(df)} MAC BLER data points ({perspective})")

    # Load LDPC data
    if ldpc_csv.exists():
        df_ldpc = pd.read_csv(ldpc_csv)
        print(f"Loaded {len(df_ldpc)} LDPC data points ({perspective})")
    else:
        print("Warning: No LDPC data found")
        df_ldpc = pd.DataFrame()

    # Create figure with 4 subplots
    fig = plt.figure(figsize=(20, 10))
    gs = fig.add_gridspec(2, 2, hspace=0.25, wspace=0.35)

    mcs_values = sorted(df['mcs'].unique())
    print(f"MCS values: {mcs_values}")

    # Modulation order groups with fixed ellipse positions
    mod_groups = {
        'QPSK': {'mcs_range': (0, 9), 'color': 'blue', 'snr_center': 7, 'snr_width': 6},
        'QAM16': {'mcs_range': (10, 16), 'color': 'green', 'snr_center': 13, 'snr_width': 4},
        'QAM64': {'mcs_range': (17, 28), 'color': 'orange', 'snr_center': 20, 'snr_width': 8}
    }

    # 1. PC5 MAC BLER vs SNR (top left)
    ax1 = fig.add_subplot(gs[0, 0])

    for mcs in mcs_values:
        mcs_data = df[df['mcs'] == mcs]
        # Aggregate BLER by SNR (average across iterations)
        mcs_agg = mcs_data.groupby('snr')['bler'].mean().reset_index()
        mcs_agg = mcs_agg.sort_values('snr')

        # Use cubic spline for smooth curves if we have enough points
        if len(mcs_agg) >= 4:
            snr_smooth = np.linspace(mcs_agg['snr'].min(), mcs_agg['snr'].max(), 300)
            spline = make_interp_spline(mcs_agg['snr'], mcs_agg['bler'], k=3)
            bler_smooth = spline(snr_smooth)
            # Clip to valid BLER range [0, 1]
            bler_smooth = np.clip(bler_smooth, 0, 1)
            ax1.plot(snr_smooth, bler_smooth, label=f'MCS {int(mcs)}', linewidth=2.5)
        else:
            # Fall back to linear for insufficient data
            ax1.plot(mcs_agg['snr'], mcs_agg['bler'],
                     marker='o', label=f'MCS {int(mcs)}', linewidth=2.5, markersize=7)

    # Add ellipses for modulation order groups with fixed positions
    for mod_name, params in mod_groups.items():
        mcs_min, mcs_max = params['mcs_range']
        color = params['color']
        snr_center = params['snr_center']
        snr_width = params['snr_width']

        # Check if this modulation group has data
        group_mcs = [m for m in mcs_values if mcs_min <= m <= mcs_max]
        if not group_mcs:
            continue

        # Fixed positioning
        bler_center = 0.5  # Vertically centered
        bler_height = 0.5  # Reduced height

        ellipse = Ellipse((snr_center, bler_center), snr_width, bler_height,
                         facecolor='none', edgecolor=color, linewidth=2, linestyle='--', alpha=0.5)
        ax1.add_patch(ellipse)

        # Add modulation order label
        ax1.text(snr_center, bler_center, mod_name,
                ha='center', va='center', fontsize=10, fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.3', facecolor='white', edgecolor=color, alpha=0.7))

    ax1.set_xlabel('SNR (dB)', fontsize=14, fontweight='bold')
    ax1.set_ylabel('Block Error Rate', fontsize=14, fontweight='bold')
    ax1.set_title(subplot_title, fontsize=16, fontweight='bold')
    ax1.grid(True, alpha=0.3, linewidth=1)
    # Compact legend with 2 columns, smaller font, positioned right at plot edge
    ax1.legend(bbox_to_anchor=(1.01, 1), loc='upper left', fontsize=6.5, ncol=2)
    ax1.set_ylim(0, 1.0)
    ax1.tick_params(labelsize=11)

    # 2. HARQ Rounds by MCS (top right) - Normalized
    ax2 = fig.add_subplot(gs[0, 1])
    mcs_groups = df.groupby('mcs').agg({
        'rounds_0': 'sum',
        'rounds_1': 'sum',
        'rounds_2': 'sum',
        'rounds_3': 'sum'
    })

    # Normalize to percentages
    mcs_groups_pct = mcs_groups.astype(float).copy()
    for mcs in mcs_values:
        total = (mcs_groups.loc[mcs, 'rounds_0'] +
                 mcs_groups.loc[mcs, 'rounds_1'] +
                 mcs_groups.loc[mcs, 'rounds_2'] +
                 mcs_groups.loc[mcs, 'rounds_3'])
        if total > 0:
            mcs_groups_pct.loc[mcs, 'rounds_0'] = (mcs_groups.loc[mcs, 'rounds_0'] / total) * 100
            mcs_groups_pct.loc[mcs, 'rounds_1'] = (mcs_groups.loc[mcs, 'rounds_1'] / total) * 100
            mcs_groups_pct.loc[mcs, 'rounds_2'] = (mcs_groups.loc[mcs, 'rounds_2'] / total) * 100
            mcs_groups_pct.loc[mcs, 'rounds_3'] = (mcs_groups.loc[mcs, 'rounds_3'] / total) * 100

    x = np.arange(len(mcs_values))
    width = 0.6

    # Stacked bar chart for normalized data
    bars1 = ax2.bar(x, mcs_groups_pct.loc[mcs_values, 'rounds_0'],
                    width, label='Round 0', color='green', alpha=0.8, edgecolor='black', linewidth=0.5)
    bars2 = ax2.bar(x, mcs_groups_pct.loc[mcs_values, 'rounds_1'],
                    width, bottom=mcs_groups_pct.loc[mcs_values, 'rounds_0'],
                    label='Round 1', color='yellow', alpha=0.8, edgecolor='black', linewidth=0.5)
    bars3 = ax2.bar(x, mcs_groups_pct.loc[mcs_values, 'rounds_2'],
                    width, bottom=(mcs_groups_pct.loc[mcs_values, 'rounds_0'] +
                                   mcs_groups_pct.loc[mcs_values, 'rounds_1']),
                    label='Round 2', color='orange', alpha=0.8, edgecolor='black', linewidth=0.5)
    bars4 = ax2.bar(x, mcs_groups_pct.loc[mcs_values, 'rounds_3'],
                    width, bottom=(mcs_groups_pct.loc[mcs_values, 'rounds_0'] +
                                   mcs_groups_pct.loc[mcs_values, 'rounds_1'] +
                                   mcs_groups_pct.loc[mcs_values, 'rounds_2']),
                    label='Round 3', color='red', alpha=0.8, edgecolor='black', linewidth=0.5)

    ax2.set_xlabel('MCS', fontsize=14, fontweight='bold')
    ax2.set_ylabel('Tx Distribution (%)', fontsize=14, fontweight='bold')
    ax2.set_title('HARQ Rounds Distribution by MCS (Normalized)', fontsize=16, fontweight='bold')
    ax2.set_xticks(x)
    ax2.set_xticklabels([int(m) for m in mcs_values])
    ax2.legend(fontsize=11, loc='upper left')
    ax2.grid(True, alpha=0.3, axis='y', linewidth=1)
    ax2.set_ylim(0, 100)
    ax2.tick_params(labelsize=11)

    # 3. SNR for 10% BLER (bottom left)
    ax3 = fig.add_subplot(gs[1, 0])
    snr_10_bler = []
    mcs_for_plot = []
    for mcs in mcs_values:
        mcs_data = df[df['mcs'] == mcs].sort_values('snr')
        bler_10 = mcs_data[mcs_data['bler'] <= 0.10]
        if len(bler_10) > 0:
            snr_threshold = bler_10['snr'].min()
            snr_10_bler.append(snr_threshold)
            mcs_for_plot.append(int(mcs))

    if snr_10_bler:
        ax3.plot(mcs_for_plot, snr_10_bler, marker='o', color='blue',
                 linewidth=2.5, markersize=11)
        # Add value labels only for every 4th MCS to reduce clutter
        for i, (mcs, snr) in enumerate(zip(mcs_for_plot, snr_10_bler)):
            if i % 4 == 0 or i == len(mcs_for_plot) - 1:
                ax3.text(mcs, snr + 0.3, f'{snr:.1f}', ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax3.set_xlabel('MCS', fontsize=14, fontweight='bold')
    ax3.set_ylabel('SNR Threshold (dB)', fontsize=14, fontweight='bold')
    ax3.set_title('SNR Required for 10% BLER Target', fontsize=16, fontweight='bold')
    ax3.grid(True, alpha=0.3, linewidth=1)
    ax3.set_xticks([m for i, m in enumerate(mcs_for_plot) if i % 4 == 0 or i == len(mcs_for_plot) - 1])
    ax3.tick_params(labelsize=11)

    # 4. LDPC Iterations Heatmap (bottom right)
    ax4 = fig.add_subplot(gs[1, 1])

    if not df_ldpc.empty:
        # Create heatmap of average LDPC iterations
        pivot_ldpc = df_ldpc.pivot_table(
            values='avg_ldpc_iter',
            index='mcs',
            columns='noise',
            aggfunc='mean'
        )

        im = ax4.imshow(pivot_ldpc.values, aspect='auto', cmap='YlOrRd',
                        interpolation='nearest', vmin=1, vmax=8)

        # Reduce label density
        x_indices = np.arange(len(pivot_ldpc.columns))
        x_labels = [int(n) if i % 2 == 0 else '' for i, n in enumerate(pivot_ldpc.columns)]
        ax4.set_xticks(x_indices)
        ax4.set_xticklabels(x_labels, fontsize=9)

        y_indices = np.arange(len(pivot_ldpc.index))
        y_labels = [int(m) if i % 2 == 0 else '' for i, m in enumerate(pivot_ldpc.index)]
        ax4.set_yticks(y_indices)
        ax4.set_yticklabels(y_labels, fontsize=9)

        ax4.set_xlabel('Noise Power (dB)', fontsize=14, fontweight='bold')
        ax4.set_ylabel('MCS', fontsize=14, fontweight='bold')
        ax4.set_title('LDPC Decoder Iterations (Average)', fontsize=16, fontweight='bold')

        cbar = plt.colorbar(im, ax=ax4)
        cbar.set_label('Avg Iterations', fontsize=12, fontweight='bold')
        cbar.ax.tick_params(labelsize=10)
    else:
        ax4.text(0.5, 0.5, 'No LDPC data available',
                ha='center', va='center', fontsize=14, color='red', fontweight='bold',
                transform=ax4.transAxes)
        ax4.set_title('LDPC Decoder Iterations (Average)', fontsize=16, fontweight='bold')

    fig.suptitle(f'{main_title}\nMCS: 0 to 28, Noise Power: -12 to 4 dB',
                 fontsize=18, fontweight='bold', y=0.995, ha='center')

    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"\n✓ Plot saved: {output_file}")

def main():
    if len(sys.argv) < 2:
        print("Usage: plot_bler_4panel.py <results_dir> [perspective]")
        print("  perspective: 'nearby' (default) or 'syncref_rx'")
        sys.exit(1)

    results_dir = sys.argv[1]
    perspective = sys.argv[2] if len(sys.argv) >= 3 else 'nearby'

    plot_bler_4panel(results_dir, perspective)

if __name__ == '__main__':
    main()
