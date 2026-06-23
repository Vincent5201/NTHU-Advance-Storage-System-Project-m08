#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <iomanip>
#include <queue>
#include <fstream>
#include <string>
#include <numeric> 

using namespace std;

const double penalty_k = 2;
const double target_memory_bits = 680.0; 

// Basic simulation parameters
const double base_latency = 100.0;
const double empty_query_ratio = 1;
const double hotness_factor = 0.2;

const int layers = 4;
const double T = 4.0;
const int num = 1000000;

// Keep original low traffic settings
const double request_arrival_rate = 1.6;   
const int parallel_channels = 8;

const double ln2_sq = log(2.0) * log(2.0);

// SSTable structure
struct SSTable {
    bool is_low_ssd = false;
    double retry_prob = 0.0;
    double ssd_latency_factor = 1.0;
    double calculated_bi = 0.0;
    double fpr = 0.0;
    double c_value = 1.0; 
    
    long long fp_count = 0;
    long long intercept_count = 0;
};

struct Layer {
    double capacity;      
    double data_prob;     
    vector<SSTable> sstables; 
};

struct RequestEvent {
    double arrival_time;   
    double service_time;   
    double completion_time;
    
    bool operator>(const RequestEvent& other) const {
        return arrival_time > other.arrival_time;
    }
};

struct SimulationResult {
    string mode_name;
    double last_layer_retry_prob;
    double current_low_ssd;
    double c_l3;
    vector<double> layer_bis;  
    vector<double> layer_fprs; 
    double total_used_bits;
    double mean_latency; 
    double p50;
    double p99;
    double p99_9;
    double p99_99;
    double throughput;
    vector<long long> layer_fps;        
    vector<long long> layer_intercepts; 
};

class LSMTreeSimulator {
private:
    int num_layers;
    double T_factor;
    vector<Layer> layer_vec;
    mt19937 gen;

    double get_read_latency(const SSTable& sst) {
        double latency = base_latency * sst.ssd_latency_factor;
        
        // Added jitter back as requested! (5.0us hardware micro-vibration)
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
        
        // As requested: intentionally don't multiply by penalty_k, maintain physical 1x penalty
        return current_latency + (retry_steps * latency); 
    }

    double run_single_read_service_time() {
        double total_time = 0;
        uniform_real_distribution<double> dist(0.0, 1.0);
        bool is_empty_query = (dist(gen) < empty_query_ratio);

        int actual_layer = -1;
        if (!is_empty_query) {
            double rand_val = dist(gen);
            double cumulative_prob = 0;
            for (int i = 0; i < num_layers; ++i) {
                cumulative_prob += layer_vec[i].data_prob;
                if (rand_val <= cumulative_prob) {
                    actual_layer = i;
                    break;
                }
            }
        }

        for (int i = 0; i < num_layers; ++i) {
            bool is_data_here = (actual_layer == i);
            
            uniform_int_distribution<int> sst_dist(0, layer_vec[i].sstables.size() - 1);
            int picked_idx = sst_dist(gen);
            SSTable& target_sst = layer_vec[i].sstables[picked_idx];

            bool bf_says_yes = is_data_here;
            if (!is_data_here) {
                if (dist(gen) < target_sst.fpr) {
                    bf_says_yes = true;
                    target_sst.fp_count++; 
                } else {
                    target_sst.intercept_count++; 
                }
            }

            if (bf_says_yes) {
                total_time += get_read_latency(target_sst);
                if (is_data_here) break;
            }
        }
        return total_time;
    }

public:
    double actual_used_bits; 
    double calculated_c_l3 = 1.0; 

    LSMTreeSimulator(int layers_count, double growth_factor, const vector<SSTable>& pre_shuffled_sstables) 
        : num_layers(layers_count), T_factor(growth_factor), gen(random_device{}()) {
        
        int sst_offset = 0;
        double total_weight = 0.0;
        for (int i = 0; i < num_layers; ++i) {
            double cap = pow(T_factor, i);
            double weight = cap * pow(hotness_factor, i);
            
            Layer l;
            l.capacity = cap;
            l.data_prob = weight; 
            
            int sst_count = static_cast<int>(cap);
            for (int j = 0; j < sst_count; ++j) {
                l.sstables.push_back(pre_shuffled_sstables[sst_offset++]);
            }
            layer_vec.push_back(l);
            total_weight += weight;
        }

        for (auto& l : layer_vec) {
            l.data_prob /= total_weight;
        }
        actual_used_bits = 0;
    }

    void setup_bloom_filters(int mode, double total_memory_bits) {
        double N_total = 85.0; 

        if (mode == 1) {
            double b_uniform = total_memory_bits / N_total;
            for (int i = 0; i < num_layers; ++i) {
                for (auto& sst : layer_vec[i].sstables) {
                    sst.calculated_bi = b_uniform;
                    sst.fpr = exp(-b_uniform * ln2_sq);
                }
            }
            calculated_c_l3 = 1.0;
        } 
        else {
            vector<double> c_high(num_layers, 1.0);
            vector<double> c_low(num_layers, 1.0);
            vector<int> count_high(num_layers, 0);
            vector<int> count_low(num_layers, 0);

            for (int i = 0; i < num_layers; ++i) {
                for (const auto& sst : layer_vec[i].sstables) {
                    if (sst.is_low_ssd) {
                        count_low[i]++;
                        c_low[i] = (mode == 3) ? sst.c_value : 1.0;
                    } else {
                        count_high[i]++;
                        c_high[i] = (mode == 3) ? sst.c_value : 1.0;
                    }
                }
            }

            double sum_monkey_term = 0.0;
            for (int i = 0; i < num_layers; ++i) {
                if (count_high[i] > 0) {
                    sum_monkey_term += count_high[i] * log(layer_vec[i].capacity / c_high[i]);
                }
                if (count_low[i] > 0) {
                    sum_monkey_term += count_low[i] * log(layer_vec[i].capacity / c_low[i]);
                }
            }

            for (int i = 0; i < num_layers; ++i) {
                double b_layer_high = (total_memory_bits / N_total) 
                                    + (sum_monkey_term / (N_total * ln2_sq)) 
                                    - (log(layer_vec[i].capacity / c_high[i]) / ln2_sq);
                
                double b_layer_low = (total_memory_bits / N_total) 
                                   + (sum_monkey_term / (N_total * ln2_sq)) 
                                   - (log(layer_vec[i].capacity / c_low[i]) / ln2_sq);

                if (b_layer_high < 0) b_layer_high = 0.0;
                if (b_layer_low < 0) b_layer_low = 0.0;

                for (auto& sst : layer_vec[i].sstables) {
                    if (sst.is_low_ssd) {
                        sst.calculated_bi = b_layer_low;
                    } else {
                        sst.calculated_bi = b_layer_high;
                    }
                    sst.fpr = exp(-sst.calculated_bi * ln2_sq);
                }
            }

            double l3_cost_sum = 0;
            for (const auto& sst : layer_vec[num_layers - 1].sstables) {
                l3_cost_sum += (mode == 3) ? sst.c_value : 1.0;
            }
            calculated_c_l3 = l3_cost_sum / layer_vec[num_layers - 1].capacity;
        }

        actual_used_bits = 0;
        for (int i = 0; i < num_layers; ++i) {
            for (const auto& sst : layer_vec[i].sstables) {
                actual_used_bits += sst.calculated_bi;
            }
        }
    }

    pair<vector<double>, vector<double>> get_layer_details() {
        vector<double> bis, fprs;
        for (int i = 0; i < num_layers; ++i) {
            double sum_bi = 0, sum_fpr = 0;
            for (const auto& sst : layer_vec[i].sstables) {
                sum_bi += sst.calculated_bi;
                sum_fpr += sst.fpr;
            }
            bis.push_back(sum_bi / layer_vec[i].capacity);
            fprs.push_back((sum_fpr / layer_vec[i].capacity) * 100.0); 
        }
        return {bis, fprs};
    }

    SimulationResult simulate_with_queue(int queries, double arrival_rate, int num_channels, string mode_name, double retry_p3, double low_ssd) {
        vector<double> arrival_times(queries);
        exponential_distribution<double> arrival_dist(arrival_rate);
        
        double current_arrival_time = 0.0;
        for (int i = 0; i < queries; ++i) {
            current_arrival_time += arrival_dist(gen); 
            arrival_times[i] = current_arrival_time;
        }

        // Physical channel queue
        priority_queue<double, vector<double>, greater<double>> channel_busy_until;
        for (int i = 0; i < num_channels; ++i) {
            channel_busy_until.push(0.0);
        }

        vector<double> total_latencies;
        total_latencies.reserve(queries);
        double max_completion_time = 0.0;
        double sum_latencies = 0.0; 

        uniform_real_distribution<double> dist(0.0, 1.0);

        vector<long long> fps(num_layers, 0), intercepts(num_layers, 0);

        for (int i = 0; i < queries; ++i) {
            double req_arrival = arrival_times[i];
            
            // Determine if query is empty and which layer contains data
            bool is_empty_query = (dist(gen) < empty_query_ratio);
            int actual_layer = -1;
            if (!is_empty_query) {
                double rand_val = dist(gen);
                double cumulative_prob = 0;
                for (int l = 0; l < num_layers; ++l) {
                    cumulative_prob += layer_vec[l].data_prob;
                    if (rand_val <= cumulative_prob) {
                        actual_layer = l;
                        break;
                    }
                }
            }

            // Track current progress cursor - which time point the request has advanced to
            double current_req_time_cursor = req_arrival; 

            // Move LSM multi-layer scan here to achieve true asynchronous decoupled queueing
            for (int l = 0; l < num_layers; ++l) {
                bool is_data_here = (actual_layer == l);
                
                uniform_int_distribution<int> sst_dist(0, layer_vec[l].sstables.size() - 1);
                int picked_idx = sst_dist(gen);
                SSTable& target_sst = layer_vec[l].sstables[picked_idx];

                bool bf_says_yes = is_data_here;
                if (!is_data_here) {
                    if (dist(gen) < target_sst.fpr) {
                        bf_says_yes = true;
                        target_sst.fp_count++; 
                        fps[l]++;
                    } else {
                        target_sst.intercept_count++; 
                        intercepts[l]++;
                    }
                }

                // If blocked by Bloom Filter (bf_says_yes == false),
                // skip directly with 0 cost, do not advance cursor, check next layer
                if (bf_says_yes) {
                    // 1. Calculate single SSTable physical I/O cost (including retries)
                    double physical_io_duration = get_read_latency(target_sst);
                    
                    // 2. Get current first-available disk channel
                    double earliest_free_time = channel_busy_until.top();
                    channel_busy_until.pop();

                    // 3. I/O start time = max(request progress, channel availability)
                    double io_start_time = max(current_req_time_cursor, earliest_free_time);
                    double io_completion_time = io_start_time + physical_io_duration;

                    // 4. Update channel busy status and request time progress cursor
                    channel_busy_until.push(io_completion_time);
                    current_req_time_cursor = io_completion_time; 

                    // If data truly found, break LSM search early
                    if (is_data_here) break;
                }
            }

            // Final latency = request end cursor time - initial arrival time
            double full_latency = current_req_time_cursor - req_arrival;
            total_latencies.push_back(full_latency);
            sum_latencies += full_latency; 

            if (current_req_time_cursor > max_completion_time) {
                max_completion_time = current_req_time_cursor;
            }
        }

        double mean_latency = (queries > 0) ? (sum_latencies / queries) : 0.0; 
        sort(total_latencies.begin(), total_latencies.end());
        auto get_p = [&](double p) { return total_latencies[int(queries * p / 100.0)]; };

        auto details = get_layer_details();
        double throughput = (max_completion_time > 0) ? (queries / max_completion_time) : 0;

        return {
            mode_name, retry_p3, low_ssd, calculated_c_l3,
            details.first, details.second, actual_used_bits, 
            mean_latency, get_p(50), get_p(99), get_p(99.9), get_p(99.99),
            throughput, fps, intercepts
        };
    }
};

int main() {
    vector<SimulationResult> all_results;

    const double retry_min = 0.01;
    const double retry_max = 0.40;
    const double ssd_min = 1.0;
    const double ssd_max = 1.0;

    const int retry_steps = 15;
    const int ssd_steps = 1;
    const int total_random_groups = retry_steps * ssd_steps; 

    cout << "Channels = " << parallel_channels << ", Arrival Rate = " << request_arrival_rate << "\n";
    cout << "Generating " << total_random_groups << " uniform grid (retry, low_ssd) groups...\n\n";

    // Global SSD heterogeneity map - fixed once to determine which 25 of 85 SSTables are low-SSD devices
    vector<int> global_bad_map(85, 0);
    for (int k = 0; k < 25; ++k) global_bad_map[k] = 1; 
    
    mt19937 global_gen(random_device{}());
    shuffle(global_bad_map.begin(), global_bad_map.end(), global_gen); 

    int g = 0;
    for (int i = 0; i < retry_steps; ++i) {
        double p3 = (retry_steps > 1) 
                    ? retry_min + (retry_max - retry_min) * i / (retry_steps - 1)
                    : retry_min;
        
        for (int j = 0; j < ssd_steps; ++j) {
            double current_low_ssd = (ssd_steps > 1)
                                     ? ssd_min + (ssd_max - ssd_min) * j / (ssd_steps - 1)
                                     : ssd_min;
            
            g++;
            cout << "[" << g << "/" << total_random_groups << "] Simulating grid retry_p3 = " 
                 << fixed << setprecision(4) << p3 << ", low_ssd = " << current_low_ssd << endl;

            // Initialize the 85 SSTables securely based on current p3 parameters and the global map shuffle
            vector<SSTable> step_sstables(85);
            for (int k = 0; k < 85; ++k) {
                if (global_bad_map[k] == 1) {
                    step_sstables[k].is_low_ssd = true;
                    step_sstables[k].retry_prob = p3; 
                    step_sstables[k].ssd_latency_factor = current_low_ssd;
                    step_sstables[k].c_value = (1.0 + (p3 / (1.0 - p3)) * penalty_k) * current_low_ssd;
                } else {
                    step_sstables[k].is_low_ssd = false;
                    step_sstables[k].retry_prob = 0.001; 
                    step_sstables[k].ssd_latency_factor = 1.0;
                    step_sstables[k].c_value = (1.0 + (0.001 / (1.0 - 0.001)) * penalty_k) * 1.0;
                }
            }

            {
                LSMTreeSimulator sim(layers, T, step_sstables);
                sim.setup_bloom_filters(1, target_memory_bits);
                all_results.push_back(sim.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 1 (Uniform)", p3, current_low_ssd));
            }

            {
                LSMTreeSimulator sim(layers, T, step_sstables);
                sim.setup_bloom_filters(2, target_memory_bits);
                all_results.push_back(sim.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 2 (Monkey)", p3, current_low_ssd));
            }

            {
                LSMTreeSimulator sim(layers, T, step_sstables);
                sim.setup_bloom_filters(3, target_memory_bits);
                all_results.push_back(sim.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 3 (Age-aware)", p3, current_low_ssd));
            }
        }
    }

    // Output to CSV
    string filename = "lsm_results.csv";
    ofstream csv_file(filename);
    
    if (csv_file.is_open()) {
        csv_file << "Mode,Layer3_Retry_Prob,Low_SSD,C_L3,"
                 << "b_L0,b_L1,b_L2,b_L3,"
                 << "FPR_L0(%),FPR_L1(%),FPR_L2(%),FPR_L3(%),"
                 << "Total_Used_Bits,"
                 << "Mean_Latency_us," 
                 << "P50_us,P99_us,P99.9_us,P99.99_us,"
                 << "Throughput,"
                 << "FP_L0,FP_L1,FP_L2,FP_L3,"                  
                 << "Intercept_L0,Intercept_L1,Intercept_L2,Intercept_L3\n"; 
        
        for (const auto& res : all_results) {
            csv_file << res.mode_name << "," << res.last_layer_retry_prob << "," << res.current_low_ssd << "," << res.c_l3 << ",";
            for (double b : res.layer_bis) csv_file << b << ",";
            for (double f : res.layer_fprs) csv_file << f << ",";
            csv_file << res.total_used_bits << ","
                     << res.mean_latency << "," 
                     << res.p50 << "," << res.p99 << "," << res.p99_9 << "," << res.p99_99 << ","
                     << res.throughput << ",";
            
            for (long long fp : res.layer_fps) csv_file << fp << ",";
            for (size_t i = 0; i < res.layer_intercepts.size(); ++i) {
                csv_file << res.layer_intercepts[i] << (i == res.layer_intercepts.size() - 1 ? "" : ",");
            }
            csv_file << "\n";
        }
        csv_file.close();
        cout << "\nSuccessfully exported data to: " << filename << endl;
    } else {
        cerr << "Failed to open CSV file for writing." << endl;
    }

    return 0;
}