#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <iomanip>
#include <queue>
#include <fstream>
#include <string>

using namespace std;

const double penalty_k = 2;
const double target_memory_bits = 680.0; 
const double base_latency = 100.0;
const double hotness_factor = 0.20;
const int layers = 4;
const double T = 4.0;
const int num = 1000000;

const double request_arrival_rate = 0.05;   
const int parallel_channels = 1;
const double ln2_sq = log(2.0) * log(2.0);

struct SSTable {
    bool is_low_ssd = false;
    double retry_prob = 0.0;
    double ssd_latency_factor = 1.0;
    double calculated_bi = 0.0;
    double fpr = 0.0;
    double c_value = 1.0; 
};

struct Layer {
    double capacity;      
    double data_prob;     
    vector<SSTable> sstables; 
};

struct SimulationResult {
    string mode_name;
    double empty_query_ratio; 
    double total_used_bits; // Added: total memory check
    double mean_latency; 
    double p50;
    double p99;
    double p99_9; 
    double p99_99;
};

class LSMTreeSimulator {
private:
    int num_layers;
    double T_factor;
    double empty_query_ratio; 
    vector<Layer> layer_vec;
    mt19937 gen;

    double get_read_latency(const SSTable& sst) {
        double latency = base_latency * sst.ssd_latency_factor;

        normal_distribution<double> jitter_dist(latency, 5.0);
        double physical_floor = 10.0;
        double current_latency = max(physical_floor, jitter_dist(gen));
        
        uniform_real_distribution<double> prob_dist(0.0, 1.0);
        int retry_steps = 0;
        bool is_error = (prob_dist(gen) < sst.retry_prob);
        while (is_error) { 
            retry_steps++; 
            is_error = (prob_dist(gen) < sst.retry_prob); 
        }
        return current_latency + (retry_steps * latency); 
    }

public:
    double actual_used_bits = 0.0;

    LSMTreeSimulator(int layers_count, double growth_factor, double f, const vector<SSTable>& pre_shuffled_sstables) 
        : num_layers(layers_count), T_factor(growth_factor), empty_query_ratio(f), gen(random_device{}()) {
        int sst_offset = 0;
        double total_weight = 0.0;
        for (int i = 0; i < num_layers; ++i) {
            double cap = pow(T_factor, i);
            double weight = cap * pow(hotness_factor, i);
            Layer l; l.capacity = cap; l.data_prob = weight; 
            for (int j = 0; j < static_cast<int>(cap); ++j) l.sstables.push_back(pre_shuffled_sstables[sst_offset++]);
            layer_vec.push_back(l);
            total_weight += weight;
        }
        for (auto& l : layer_vec) l.data_prob /= total_weight;
    }

    void setup_bloom_filters(int mode, double total_memory_bits) {
        double N_total = 85.0; 
        if (mode == 1) {
            for (int i = 0; i < num_layers; ++i) {
                for (auto& sst : layer_vec[i].sstables) {
                    sst.calculated_bi = total_memory_bits / N_total; sst.fpr = exp(-sst.calculated_bi * ln2_sq);
                }
            }
        } else {
            vector<double> c_high(num_layers, 1.0), c_low(num_layers, 1.0);
            vector<int> count_high(num_layers, 0), count_low(num_layers, 0);
            for (int i = 0; i < num_layers; ++i) {
                for (const auto& sst : layer_vec[i].sstables) {
                    if (sst.is_low_ssd) { count_low[i]++; c_low[i] = (mode == 3) ? sst.c_value : 1.0; } 
                    else { count_high[i]++; c_high[i] = (mode == 3) ? sst.c_value : 1.0; }
                }
            }
            double sum_monkey_term = 0.0;
            for (int i = 0; i < num_layers; ++i) {
                if (count_high[i] > 0) sum_monkey_term += count_high[i] * log(layer_vec[i].capacity / c_high[i]);
                if (count_low[i] > 0) sum_monkey_term += count_low[i] * log(layer_vec[i].capacity / c_low[i]);
            }
            for (int i = 0; i < num_layers; ++i) {
                double b_layer_high = max(0.0, (total_memory_bits / N_total) + (sum_monkey_term / (N_total * ln2_sq)) - (log(layer_vec[i].capacity / c_high[i]) / ln2_sq));
                double b_layer_low = max(0.0, (total_memory_bits / N_total) + (sum_monkey_term / (N_total * ln2_sq)) - (log(layer_vec[i].capacity / c_low[i]) / ln2_sq));
                for (auto& sst : layer_vec[i].sstables) {
                    sst.calculated_bi = sst.is_low_ssd ? b_layer_low : b_layer_high; sst.fpr = exp(-sst.calculated_bi * ln2_sq);
                }
            }
        }

        // Calculate total bits consumed by this configuration
        actual_used_bits = 0.0;
        for (int i = 0; i < num_layers; ++i) {
            for (const auto& sst : layer_vec[i].sstables) {
                actual_used_bits += sst.calculated_bi;
            }
        }
    }

    SimulationResult simulate_with_queue(int queries, double arrival_rate, int num_channels, string mode_name) {
        vector<double> arrival_times(queries);
        exponential_distribution<double> arrival_dist(arrival_rate);
        
        double current_arrival_time = 0.0;
        for (int i = 0; i < queries; ++i) {
            current_arrival_time += arrival_dist(gen); 
            arrival_times[i] = current_arrival_time;
        }

        priority_queue<double, vector<double>, greater<double>> channel_busy_until;
        for (int i = 0; i < num_channels; ++i) channel_busy_until.push(0.0);

        vector<double> total_latencies;
        total_latencies.reserve(queries);
        double sum_latencies = 0.0; 

        uniform_real_distribution<double> dist(0.0, 1.0);

        for (int i = 0; i < queries; ++i) {
            double req_arrival = arrival_times[i];
            
            bool is_empty_query = (dist(gen) < empty_query_ratio);
            int actual_layer = -1;
            if (!is_empty_query) {
                double rand_val = dist(gen);
                double cumulative_prob = 0;
                for (int l = 0; l < num_layers; ++l) {
                    cumulative_prob += layer_vec[l].data_prob;
                    if (rand_val <= cumulative_prob) { actual_layer = l; break; }
                }
            }

            double current_req_time_cursor = req_arrival; 

            for (int l = 0; l < num_layers; ++l) {
                bool is_data_here = (actual_layer == l);
                
                // Fix: Lock the same SSTable entity before checking and queuing to avoid space-time separation
                uniform_int_distribution<int> sst_dist(0, layer_vec[l].sstables.size() - 1);
                SSTable& target_sst = layer_vec[l].sstables[sst_dist(gen)];

                bool bf_says_yes = is_data_here;
                if (!is_data_here) { 
                    if (dist(gen) < target_sst.fpr) bf_says_yes = true; 
                }

                if (bf_says_yes) {
                    double physical_io_duration = get_read_latency(target_sst);
                    
                    double earliest_free_time = channel_busy_until.top();
                    channel_busy_until.pop();

                    double io_start_time = max(current_req_time_cursor, earliest_free_time);
                    double io_completion_time = io_start_time + physical_io_duration;

                    channel_busy_until.push(io_completion_time);
                    current_req_time_cursor = io_completion_time;

                    // Fix: If data is truly found in this layer, must break early to stop searching subsequent layers!
                    if (is_data_here) break; 
                }
            }

            double full_latency = current_req_time_cursor - req_arrival;
            total_latencies.push_back(full_latency);
            sum_latencies += full_latency; 
        }

        double mean_latency = sum_latencies / queries; 
        sort(total_latencies.begin(), total_latencies.end());

        return { mode_name, empty_query_ratio, actual_used_bits, mean_latency, total_latencies[int(queries * 0.5)], total_latencies[int(queries * 0.99)], total_latencies[int(queries * 0.999)], total_latencies[int(queries * 0.9999)] };
    }
};

int main() {
    vector<SimulationResult> all_results;
    
    const double fixed_p3 = 0.20; 
    const int f_steps = 21;
    const double f_start = 1.0;
    const double f_end = 0.80;

    vector<int> global_bad_map(85, 0);
    for (int k = 0; k < 25; ++k) global_bad_map[k] = 1; 
    mt19937 global_gen(random_device{}());
    shuffle(global_bad_map.begin(), global_bad_map.end(), global_gen);

    vector<SSTable> global_sstables(85);
    for (int k = 0; k < 85; ++k) {
        if (global_bad_map[k] == 1) {
            global_sstables[k].is_low_ssd = true; global_sstables[k].retry_prob = fixed_p3;
            global_sstables[k].c_value = 1.0 + (fixed_p3 / (1.0 - fixed_p3)) * penalty_k;
        } else {
            global_sstables[k].retry_prob = 0.001; global_sstables[k].c_value = 1.0;
        }
    }

    for (int i = 0; i < f_steps; ++i) {
        double current_f = f_start - i * ((f_start - f_end) / (f_steps - 1));
        cout << "Simulating Empty Query Ratio f = " << fixed << setprecision(3) << current_f << "..." << endl;

        LSMTreeSimulator sim1(layers, T, current_f, global_sstables);
        sim1.setup_bloom_filters(1, target_memory_bits);
        all_results.push_back(sim1.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 1"));

        LSMTreeSimulator sim2(layers, T, current_f, global_sstables);
        sim2.setup_bloom_filters(2, target_memory_bits);
        all_results.push_back(sim2.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 2"));

        LSMTreeSimulator sim3(layers, T, current_f, global_sstables);
        sim3.setup_bloom_filters(3, target_memory_bits);
        all_results.push_back(sim3.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 3"));
    }

    // Output field: added Total_Used_Bits
    ofstream csv("lsm_results.csv");
    csv << "Mode,Empty_Query_Ratio,Total_Used_Bits,Mean,P50,P99,P99.9,P99.99\n"; 
    for (const auto& r : all_results) {
        csv << r.mode_name << "," 
            << r.empty_query_ratio << "," 
            << r.total_used_bits << "," 
            << r.mean_latency << "," 
            << r.p50 << "," << r.p99 << "," << r.p99_9 << "," << r.p99_99 << "\n";
    }
    csv.close();
    cout << "Done! Saved to lsm_results.csv\n";
    return 0;
}