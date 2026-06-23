# Heterogeneous-SSD Device-Aware Index Allocation

## Project Overview

This project implements a simulation framework for **Device-Aware Bloom Filter Index Allocation** in Log-Structured Merge (LSM) Trees with heterogeneous SSDs. The research investigates three distinct allocation strategies:

1. **Mode 1 (Uniform)**: Uniform bits per key allocation across all layers.
2. **Mode 2 (Monkey)**: Monkey algorithm-based cost-aware allocation.
3. **Mode 3 (Age-aware Monkey)**: An age-aware enhancement to the Monkey algorithm that explicitly incorporates device performance characteristics.

The framework simulates query latency, throughput, and false positive rates under varying configurations of empty query ratios, device latencies, and SSD failure profiles.

---

## Key Simulation Parameters

### LSM Tree Structure & Workload

* **Number of Layers**: 4 layers with a growth factor (T) of 4.0.
* **Total SSTables**: 85 tables distributed across the 4 layers (1, 4, 16, and 64 respectively).
* **Hotness Factor**: 0.2, modeling an exponential decay in data access probability across deeper layers.
* **Query Workload**: 1,000,000 key lookups per simulation run, driven by an exponential inter-arrival time model.
* **Empty Query Ratio**: Evaluated statically or via a parameter sweep ranging from 1.0 down to 0.80 across 21 steps.

### Device & Execution Characteristics

* **Base Performance**: Base latency of 100.0 μs, physical floor latency of 10.0 μs, and normal distribution jitter ($\sigma = 5.0\ \mu\text{s}$).
* **Failure & Latency Model**: Geometric distribution representing retry probabilities. The Layer 3 retry probability is either evaluated at a fixed baseline (0.20) or swept from 0.01 to 0.40 across 15 steps.
* **Hardware Heterogeneity**: Supports uniform layer characteristics or a randomized distribution where 25 out of the 85 SSTables are designated as low-performance SSD devices.
* **Parallelism**: Configurable execution using either a single channel or 8 parallel channels depending on the simulation target.

### Memory Configuration

* **Total Bloom Filter Memory**: Budget ranges between 680 and 850 bits total depending on the experiment configuration.

---

## How to Use

### Step 1: Build and Run Simulations

Compile the C++ simulation sources (requires C++11 or later):

```bash
g++ -O2 -std=c++11 lsm_sim_4_empty.cpp -o lsm_sim_4_empty
g++ -O2 -std=c++11 lsm_sim_4.cpp -o lsm_sim_4
g++ -O2 -std=c++11 lsm_sim_rand_empty.cpp -o lsm_sim_rand_empty
g++ -O2 -std=c++11 lsm_sim_rand.cpp -o lsm_sim_rand

```

Execute the compiled binaries alongside their corresponding Python visualization scripts:

```bash
# Experiment 1: Empty query ratio sweep (uniform SSD)
./lsm_sim_4_empty
python plot_empty.py

# Experiment 2: Retry probability sweep (uniform SSD)
./lsm_sim_4
python plot.py

# Experiment 3: Empty query ratio sweep (randomized SSD)
./lsm_sim_rand_empty
python plot_empty.py

# Experiment 4: Retry probability sweep (randomized SSD)
./lsm_sim_rand
python plot.py

```

### Step 2: Update File Paths in Python Scripts

Before executing the visualization scripts, update the input and output paths within `plot.py` and `plot_empty.py` to point to your local directories:

```python
# In plot_empty.py
df = pd.read_csv(r'YOUR_PATH_TO_lsm_results.csv')

# In plot.py
file_path = r'YOUR_PATH_TO_lsm_results.csv'
output_dir = r'YOUR_OUTPUT_PATH'

```

### Step 3: View Generated Plots

The scripts output high-resolution (300 DPI) visualization plots. These include individual metric performance curves over the parameter sweeps, layer-wise false positive breakdowns, and a combined 2x2 relative improvement matrix for empty query profiles.

---

## Output Format

The simulation outputs data directly into `lsm_results.csv`. Depending on the experiment run, the captured metrics include:

### Core Latency & Memory Metrics

* **Mode**: The filter allocation strategy being evaluated.
* **Empty_Query_Ratio / Layer3_Retry_Prob**: The independent variable for the sweep.
* **Total_Used_Bits**: The cumulative memory footprint used by the Bloom filters.
* **Latency Profile**: Comprehensive tracking of Mean, P50, P99, P99.9, and P99.99 execution latencies (μs).

### Detailed Layer & Efficiency Metrics

* **Bit Allocation**: Per-layer bits per key (`b_L0` to `b_L3`).
* **False Positive Dynamics**: Layer-wise false positive rate percentages (`FPR_L0` to `FPR_L3`) and raw false positive counts (`FP_L0` to `FP_L3`).
* **Operational Performance**: Total query throughput and successful intercept counts (`Intercept_L0` to `Intercept_L3`) for non-existent keys.

---

## Interpretation Guide

### Key Metrics
* Mean Latency: Represents the average query execution time across the entire simulation.
* Latency Percentiles: Median (P50) indicates typical behavior, while P99, P99.9, and P99.99 accurately capture tail latency under device degradation and retries.
