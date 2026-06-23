import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# 1. Load CSV dataset
df = pd.read_csv(r'C:\myCodes\C++\storage\simu\lsm_results.csv')

# 2. Pivot data for structured structural alignment
pivot_df = df.pivot(index='Empty_Query_Ratio', columns='Mode')

# 3. Calculate relative improvement percentages (%)
def calc_improvement(base, target):
    # Use np.where to safely avoid divide-by-zero runtime errors
    return np.where(base == 0, 0, (base - target) / base * 100)

imp_data = pd.DataFrame(index=pivot_df.index)

for metric in ['Mean', 'P50', 'P99', 'P99.9']:
    # Added: third line showing Mode 2's improvement over Mode 1 (Monkey's base power)
    imp_data[f'{metric}_M2_vs_M1'] = calc_improvement(pivot_df[metric]['Mode 1'], pivot_df[metric]['Mode 2'])
    # Original two lines
    imp_data[f'{metric}_M3_vs_M1'] = calc_improvement(pivot_df[metric]['Mode 1'], pivot_df[metric]['Mode 3'])
    imp_data[f'{metric}_M3_vs_M2'] = calc_improvement(pivot_df[metric]['Mode 2'], pivot_df[metric]['Mode 3'])

imp_data = imp_data.sort_index(ascending=False)

# 4. Draw 2x2 plot
fig, axs = plt.subplots(2, 2, figsize=(16, 11))

plots_info = [
    ('Mean', 'Mean Latency Relative Improvement (%)', axs[0, 0]),
    ('P50', 'P50 Latency Relative Improvement (%)', axs[0, 1]),
    ('P99', 'P99 Latency Relative Improvement (%)', axs[1, 0]),
    ('P99.9', 'P99.9 Latency Relative Improvement (%)', axs[1, 1])
]

# Define colors for academic-level visualization
color_m2_m1 = '#2ca02c'  # Green: Mode 2 vs Mode 1
color_m3_m1 = '#1f77b4'  # Blue: Mode 3 vs Mode 1
color_m3_m2 = '#ff7f0e'  # Orange: Mode 3 vs Mode 2

for metric, title, ax in plots_info:
    
    # Draw three lines
    ax.plot(imp_data.index, imp_data[f'{metric}_M2_vs_M1'], marker='^', linewidth=2, linestyle='--', color=color_m2_m1, label='Mode 2 vs Mode 1')
    ax.plot(imp_data.index, imp_data[f'{metric}_M3_vs_M1'], marker='o', linewidth=2.5, color=color_m3_m1, label='Mode 3 vs Mode 1')
    ax.plot(imp_data.index, imp_data[f'{metric}_M3_vs_M2'], marker='s', linewidth=2.5, color=color_m3_m2, label='Mode 3 vs Mode 2')
    
    # Beautification settings
    ax.set_xlim(1.00, 0.80) 
    ax.set_title(title, fontsize=15, fontweight='bold')
    ax.set_xlabel('Empty Query Ratio ($f$)', fontsize=12)
    ax.set_ylabel('Improvement (%)', fontsize=12)
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # Adjust legend position to avoid blocking lines
    ax.legend(loc='best', fontsize=10)

    # Lock P50 Y-axis to eliminate visual noise within 5% hardware vibration
    if metric == 'P50':
        ax.set_ylim(-10, 20) 

plt.tight_layout()

save_path = r'C:\myCodes\C++\storage\simu\lsm_relative_improvement_3lines.png'
plt.savefig(save_path, dpi=300)
print(f"Generated plot: {save_path}")

plt.show()