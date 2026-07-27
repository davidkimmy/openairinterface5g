#!/usr/bin/env python3
"""
Generate BLER plots for PC5 Sidelink and Uu interface
Supports: nearby, syncref_rx (PC5), uu_dl, uu_ul (Uu)
"""

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import sys
import os
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
        subplot_title = 'BLER vs SNR'
        has_ldpc = True
    elif perspective == 'syncref_rx':
        bler_csv = results_dir / "syncref_rx_bler_combined.csv"
        ldpc_csv = results_dir / "syncref_rx_ldpc_combined.csv"
        output_file = results_dir / "syncref_rx_bler_4panel.png"
        main_title = 'Performance Analysis of 5G SL Mode 1 (Syncref UE PC5 Rx)'
        subplot_title = 'BLER vs SNR'
        has_ldpc = True
    elif perspective == 'uu_dl_gnb':
        bler_csv = results_dir / "uu_dl_gnb_bler_combined.csv"
        ldpc_csv = None
        output_file = results_dir / "uu_dl_gnb_bler_4panel.png"
        main_title = 'Performance Analysis of Uu DL (gNB RX Perspective)'
        subplot_title = 'BLER vs SNR'
        has_ldpc = False
    elif perspective == 'uu_dl_relay':
        bler_csv = results_dir / "uu_dl_relay_bler_combined.csv"
        ldpc_csv = results_dir / "uu_dl_relay_ldpc_combined.csv"
        output_file = results_dir / "uu_dl_relay_bler_4panel.png"
        main_title = 'Performance Analysis of Uu DL (Relay UE RX Perspective)'
        subplot_title = 'BLER vs SNR'
        has_ldpc = True
    elif perspective == 'uu_dl':
        # Backward compatibility - defaults to gNB perspective
        bler_csv = results_dir / "uu_dl_bler_combined.csv"
        ldpc_csv = results_dir / "uu_dl_ldpc_combined.csv"
        output_file = results_dir / "uu_dl_bler_4panel.png"
        main_title = 'Performance Analysis of Uu DL (gNB → Relay UE)'
        subplot_title = 'BLER vs SNR'
        has_ldpc = True
    elif perspective == 'uu_ul':
        bler_csv = results_dir / "uu_ul_bler_combined.csv"
        ldpc_csv = None
        output_file = results_dir / "uu_ul_bler_2panel.png"
        main_title = 'Performance Analysis of Uu UL (Relay UE → gNB)'
        subplot_title = 'BLER vs SNR'
        has_ldpc = False
    else:
        print(f"Error: Unknown perspective '{perspective}'. Use 'nearby', 'syncref_rx', 'uu_dl_gnb', 'uu_dl_relay', 'uu_dl', or 'uu_ul'")
        sys.exit(1)

    # Load BLER data
    if not bler_csv.exists():
        print(f"Error: {bler_csv} not found")
        sys.exit(1)

    df = pd.read_csv(bler_csv)
    print(f"Loaded {len(df)} MAC BLER data points ({perspective})")

    # Load LDPC data (if applicable)
    if has_ldpc and ldpc_csv and ldpc_csv.exists():
        df_ldpc = pd.read_csv(ldpc_csv)
        print(f"Loaded {len(df_ldpc)} LDPC data points ({perspective})")
    else:
        if has_ldpc:
            print("Warning: No LDPC data found")
        df_ldpc = pd.DataFrame()

    # Create figure with 2 or 4 subplots based on perspective
    if perspective == 'uu_ul':
        # UL only has BLER + HARQ (no LDPC at gNB)
        fig = plt.figure(figsize=(20, 5))
        gs = fig.add_gridspec(1, 2, hspace=0.25, wspace=0.35, top=0.75)
    else:
        # Full 4-panel plot
        fig = plt.figure(figsize=(20, 10))
        gs = fig.add_gridspec(2, 2, hspace=0.45, wspace=0.35, top=0.88)

    mcs_values = sorted(df['mcs'].unique())
    print(f"MCS values: {mcs_values}")

    # Modulation order groups with fixed ellipse positions
    # Only draw ellipses if BLER data reaches moderate levels and not vrtsim.
    backend = os.environ.get("BLER_BACKEND", "")
    draw_ellipses = len(df) > 0 and df['bler'].max() > 0.20 and backend != "vrtsim"

    # Different ellipse sizing for Uu interface vs PC5 sidelink
    if perspective in ['uu_dl', 'uu_dl_gnb', 'uu_dl_relay']:
        # Smaller ellipses for Uu interface
        mod_groups = {
            'QPSK': {'mcs_range': (0, 9), 'color': 'blue', 'snr_center': 7, 'snr_width': 2},
            'QAM16': {'mcs_range': (10, 16), 'color': 'green', 'snr_center': 9.75, 'snr_width': 3.2},
            'QAM64': {'mcs_range': (17, 28), 'color': 'orange', 'snr_center': 13.3, 'snr_width': 3.2}
        }
    elif perspective in ['uu_ul']:
        # Smaller ellipses for Uu interface
        mod_groups = {
            'QPSK': {'mcs_range': (0, 9), 'color': 'blue', 'snr_center': 7.75, 'snr_width': 2},
            'QAM16': {'mcs_range': (10, 16), 'color': 'green', 'snr_center': 10.0, 'snr_width': 2.5},
            'QAM64': {'mcs_range': (17, 28), 'color': 'orange', 'snr_center': 13.0, 'snr_width': 3}
        }
    else:
        # Larger ellipses for PC5 sidelink (nearby, syncref_rx)
        mod_groups = {
            'QPSK': {'mcs_range': (0, 9), 'color': 'blue', 'snr_center': 7, 'snr_width': 6},
            'QAM16': {'mcs_range': (10, 16), 'color': 'green', 'snr_center': 13, 'snr_width': 4},
            'QAM64': {'mcs_range': (17, 28), 'color': 'orange', 'snr_center': 20, 'snr_width': 8}
        }

    # 1. BLER vs SNR (top left, or left for uu_ul)
    if perspective == 'uu_ul':
        ax1 = fig.add_subplot(gs[0, 0])
    else:
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
            ax1.plot(snr_smooth, bler_smooth, label=f'MCS {int(mcs)}', linewidth=4)
        else:
            # Fall back to linear for insufficient data - larger for visibility
            ax1.plot(mcs_agg['snr'], mcs_agg['bler'],
                     marker='o', label=f'MCS {int(mcs)}', linewidth=4, markersize=12)

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

        # Only draw ellipses if BLER range is wide enough
        if draw_ellipses:
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
    # MCS legend inside the chart (upper-right, the usually-empty corner of a BLER
    # waterfall). Favor readability: a larger font and FEWER columns so the entries list
    # down more rows instead of being crammed across many columns. Use 1 column for small
    # sweeps, at most 2 columns for the full MCS range.
    n_leg = len(mcs_values)
    leg_ncol = 1 if n_leg <= 10 else 2
    # Anchor slightly below the top edge (y=0.93 in axes fraction) so there is a top margin
    # between the chart border and the legend.
    ax1.legend(loc='upper right', bbox_to_anchor=(1.0, 0.93), fontsize=8, ncol=leg_ncol,
               framealpha=0.6, handlelength=1.5, handletextpad=0.5, columnspacing=1.0,
               labelspacing=0.4, borderpad=0.4)

    # Auto-scale y-axis to zoom into data range when BLER is low
    if len(df) > 0:
        max_bler = df['bler'].max()
        if max_bler < 0.20:  # BLER < 20%, zoom in aggressively
            # Add 20% margin, round up to nearest 0.05
            y_max = min(1.0, np.ceil((max_bler * 1.2) / 0.05) * 0.05)
            ax1.set_ylim(0, max(0.1, y_max))  # Minimum 0.1 scale
        elif max_bler < 0.90:  # BLER 20-90%, moderate zoom
            y_max = min(1.0, np.ceil((max_bler * 1.1) * 10) / 10)
            ax1.set_ylim(0, y_max)
        else:
            ax1.set_ylim(0, 1.0)
    else:
        ax1.set_ylim(0, 1.0)

    # Focus the x-axis on the active SNR range (where the curves/dotted lines are),
    # so the fixed modulation-region annotations don't stretch it to a wide fixed
    # span. This keeps the view within the swept range (SNR = TX_ref - ploss - noise),
    # regardless of backend. The out-of-range ellipses are simply clipped to the axes.
    if len(df) > 0 and df['snr'].notna().any():
        snr_lo = float(df['snr'].min())
        snr_hi = float(df['snr'].max())
        if snr_hi > snr_lo:
            margin = max(1.0, 0.05 * (snr_hi - snr_lo))
            ax1.set_xlim(snr_lo - margin, snr_hi + margin)

    ax1.tick_params(labelsize=11)

    # 2. HARQ Rounds by MCS (top right, or right for uu_ul) - Aggregated Across All SNRs
    if perspective == 'uu_ul':
        ax2 = fig.add_subplot(gs[0, 1])
    else:
        ax2 = fig.add_subplot(gs[0, 1])

    # For each MCS, aggregate HARQ data across ALL SNR values
    # This shows the overall HARQ behavior for each MCS across the entire test range
    mcs_groups = pd.DataFrame(index=mcs_values, columns=['rounds_0', 'rounds_1', 'rounds_2', 'rounds_3'])
    for mcs in mcs_values:
        mcs_data = df[df['mcs'] == mcs]

        if len(mcs_data) > 0:
            # Sum HARQ rounds across ALL datapoints for this MCS (all SNRs)
            mcs_groups.loc[mcs, 'rounds_0'] = mcs_data['rounds_0'].sum()
            mcs_groups.loc[mcs, 'rounds_1'] = mcs_data['rounds_1'].sum()
            mcs_groups.loc[mcs, 'rounds_2'] = mcs_data['rounds_2'].sum()
            mcs_groups.loc[mcs, 'rounds_3'] = mcs_data['rounds_3'].sum()
        else:
            # No data at all for this MCS
            mcs_groups.loc[mcs, 'rounds_0'] = 0
            mcs_groups.loc[mcs, 'rounds_1'] = 0
            mcs_groups.loc[mcs, 'rounds_2'] = 0
            mcs_groups.loc[mcs, 'rounds_3'] = 0

    # Normalize to percentages
    mcs_groups_pct = pd.DataFrame(index=mcs_values, columns=['rounds_0', 'rounds_1', 'rounds_2', 'rounds_3']).astype(float)
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
        else:
            mcs_groups_pct.loc[mcs, 'rounds_0'] = 0
            mcs_groups_pct.loc[mcs, 'rounds_1'] = 0
            mcs_groups_pct.loc[mcs, 'rounds_2'] = 0
            mcs_groups_pct.loc[mcs, 'rounds_3'] = 0

    harq_title = 'HARQ Tx Rounds Distribution by MCS\n(Aggregated Across All SNRs)'

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
    ax2.set_title(harq_title, fontsize=15, fontweight='bold')
    ax2.set_xticks(x)
    ax2.set_xticklabels([int(m) for m in mcs_values])
    ax2.legend(fontsize=11, loc='upper left')
    ax2.grid(True, alpha=0.3, axis='y', linewidth=1)
    ax2.set_ylim(0, 100)
    ax2.tick_params(labelsize=11)

    # 3. SNR/Noise for 10% BLER Target (bottom left) - Skip for uu_ul (2-panel layout)
    if perspective != 'uu_ul':
        ax3 = fig.add_subplot(gs[1, 0])
        snr_10_bler = []
        noise_10_bler = []
        mcs_for_plot = []
        for mcs in mcs_values:
            # Aggregate across hosts/iterations FIRST (mean BLER per (snr,noise) operating
            # point), THEN threshold -- mirrors the ax1 groupby. Using raw rows here would let
            # a single low-SNR row with BLER<=0.1 from any host collapse the threshold to the
            # minimum SNR for every MCS -> a flat line even when each host's curve is not flat.
            mcs_agg = (df[df['mcs'] == mcs]
                       .groupby(['snr', 'noise'])['bler'].mean()
                       .reset_index().sort_values('snr'))
            bler_10 = mcs_agg[mcs_agg['bler'] <= 0.10]
            if len(bler_10) > 0:
                # Find the minimum SNR where the AVERAGED BLER <= 10%
                idx_min = bler_10['snr'].idxmin()
                snr_threshold = bler_10.loc[idx_min, 'snr']
                noise_threshold = bler_10.loc[idx_min, 'noise']
                snr_10_bler.append(snr_threshold)
                noise_10_bler.append(noise_threshold)
                mcs_for_plot.append(int(mcs))

        if snr_10_bler:
            ax3.plot(mcs_for_plot, snr_10_bler, marker='o', color='blue',
                     linewidth=2.5, markersize=11, label='SNR Threshold')
            # Add dual labels: SNR above point, Noise below point
            for i, (mcs, snr, noise) in enumerate(zip(mcs_for_plot, snr_10_bler, noise_10_bler)):
                if i % 4 == 0 or i == len(mcs_for_plot) - 1:
                    # SNR label above
                    ax3.text(mcs, snr + 0.4, f'{snr:.1f}dB', ha='center', va='bottom',
                            fontsize=7, fontweight='bold', color='blue')
                    # Noise label below (smaller, gray)
                    ax3.text(mcs, snr - 0.4, f'N={noise}', ha='center', va='top',
                            fontsize=6, color='gray', style='italic')

        ax3.set_xlabel('MCS', fontsize=14, fontweight='bold')
        ax3.set_ylabel('SNR Threshold (dB)', fontsize=14, fontweight='bold')
        ax3.set_title('SNR Required for 10% BLER Target\n(with corresponding Noise Power)', fontsize=15, fontweight='bold')
        ax3.grid(True, alpha=0.3, linewidth=1)
        ax3.set_xticks([m for i, m in enumerate(mcs_for_plot) if i % 4 == 0 or i == len(mcs_for_plot) - 1])
        ax3.tick_params(labelsize=11)

    # 4. LDPC Iterations Heatmap (bottom right) - Skip for uu_ul (no LDPC at gNB)
    if perspective != 'uu_ul':
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

    # Determine actual MCS and noise ranges from data
    mcs_range_str = f"{min(mcs_values)} to {max(mcs_values)}" if len(mcs_values) > 1 else f"{mcs_values[0]}" if mcs_values else "N/A"
    noise_values = sorted(df['noise'].unique())
    noise_range_str = f"{min(noise_values)} to {max(noise_values)}" if len(noise_values) > 1 else f"{noise_values[0]}" if noise_values else "N/A"

    # Adjust title position based on layout (2-panel needs different y-position)
    if perspective == 'uu_ul':
        title_y = 0.95  # Higher position with more spacing below for 2-panel layout
    else:
        title_y = 0.995  # Standard position for 4-panel layout

    fig.suptitle(f'{main_title}\nMCS: {mcs_range_str}, Noise Power: {noise_range_str} dB',
                 fontsize=18, fontweight='bold', y=title_y, ha='center')

    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"\n✓ Plot saved: {output_file}")

def main():
    if len(sys.argv) < 2:
        print("Usage: plot_results.py <results_dir> [perspective]")
        print("  perspective: 'nearby' (default), 'syncref_rx', 'uu_dl', or 'uu_ul'")
        sys.exit(1)

    results_dir = sys.argv[1]
    perspective = sys.argv[2] if len(sys.argv) >= 3 else 'nearby'

    plot_bler_4panel(results_dir, perspective)

if __name__ == '__main__':
    main()
