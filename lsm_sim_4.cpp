#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <iomanip>
#include <queue>
#include <fstream>
#include <string>
#include <numeric> // For calculation or accumulation

using namespace std;

const double penalty_k = 2;
const double target_memory_bits = 850.0; 

// Basic simulation parameters
const double base_latency = 100.0;
const double empty_query_ratio = 1;
const double hotness_factor = 0.2;

const int layers = 4;
const double T = 4.0;
const int num = 1000000;

const double request_arrival_rate = 2;   
const int parallel_channels = 8;

const double ln2_sq = log(2.0) * log(2.0);

struct Layer {
    double capacity;
    double data_prob;
    double retry_prob; 
    double fpr;
    double calculated_bi; 
    long long fp_count = 0;
    long long intercept_count = 0;
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
    double mean_latency; // Added: average latency
    double p50;
    double p99;
    double p99_9;
    double p99_99;
    double throughput;
    vector<long long> layer_fps;        // Number of false positives per layer
    vector<long long> layer_intercepts; // Number of successful interceptions per layer
};


class LSMTreeSimulator {
private:
    int num_layers;
    double T_factor;
    vector<double> current_low_ssd_vec; 
    vector<Layer> layers;
    mt19937 gen;

    // Calculate latency for a single physical disk read (excluding queue waiting time)
    double get_read_latency(double retry_prob, int layer) {
        double latency = base_latency;
        latency *= current_low_ssd_vec[layer];
        
        normal_distribution<double> jitter_dist(latency, 5.0);
        double physical_floor = 10;
        double current_latency = max(physical_floor, jitter_dist(gen));

        uniform_real_distribution<double> prob_dist(0.0, 1.0);
        int retry_steps = 0;
        bool is_error = (prob_dist(gen) < retry_prob);
        while (is_error) {
            retry_steps++; 
            is_error = (prob_dist(gen) < retry_prob);
        }
        return current_latency + (retry_steps * latency); 
    }

public:
    double actual_used_bits; 
    double calculated_c_l3 = 1.0; 

    LSMTreeSimulator(int layers_count, double growth_factor, vector<double> raw_retry_probs, vector<double> low_ssd_vals) 
        : num_layers(layers_count), T_factor(growth_factor), current_low_ssd_vec(low_ssd_vals), gen(random_device{}()) {
        
        double total_weight = 0.0;
        for (int i = 0; i < num_layers; ++i) {
            double cap = pow(T_factor, i);
            double weight = cap * pow(hotness_factor, i);
            layers.push_back({cap, weight, raw_retry_probs[i], 0, 0, 0, 0}); 
            total_weight += weight;
        }
        for (auto& l : layers) {
            l.data_prob = l.data_prob / total_weight;
        }
        actual_used_bits = 0;
    }

    // Configures the bit allocation metrics across different modes
    void setup_bloom_filters(int mode, double total_memory_bits) {
        vector<double> b(num_layers, 0.0);
        if (mode == 1) {
            double N = 0; 
            for (auto& l : layers) N += l.capacity;
            for (int i = 0; i < num_layers; ++i) b[i] = total_memory_bits / N;
            calculated_c_l3 = 1.0; 
        } 
        else {
            vector<double> c(num_layers, 1.0);
            for (int i = 0; i < num_layers; ++i) {
                if (mode == 3) {
                    c[i] = (1.0 + (layers[i].retry_prob / (1.0 - layers[i].retry_prob)) * penalty_k) * current_low_ssd_vec[i];
                } else {
                    c[i] = 1.0;
                }
            }
            calculated_c_l3 = c[num_layers - 1]; 

            vector<bool> finalized(num_layers, false);
            double remaining_M = total_memory_bits;
            
            for (int iter = 0; iter < num_layers; ++iter) {
                double N_active = 0;
                double sum_term = 0;
                for (int j = 0; j < num_layers; ++j) {
                    if (!finalized[j]) {
                        N_active += layers[j].capacity;
                        sum_term += layers[j].capacity * log(layers[j].capacity / c[j]);
                    }
                }

                bool need_rebalance = false;
                for (int i = 0; i < num_layers; ++i) {
                    if (!finalized[i]) {
                        double b_i = (remaining_M / N_active) 
                                   + (sum_term / (N_active * ln2_sq)) 
                                   - (log(layers[i].capacity / c[i]) / ln2_sq);
                        b[i] = b_i;
                    }
                }
                if (!need_rebalance) break; 
            }
        }

        actual_used_bits = 0;
        for (int i = 0; i < num_layers; ++i) {
            layers[i].fpr = exp(-b[i] * ln2_sq);
            layers[i].calculated_bi = b[i];
            actual_used_bits += (b[i] * layers[i].capacity);
        }
    }

    pair<vector<double>, vector<double>> get_layer_details() {
        vector<double> bis, fprs;
        for (int i = 0; i < num_layers; ++i) {
            bis.push_back(layers[i].calculated_bi);
            fprs.push_back(layers[i].fpr * 100.0); 
        }
        return {bis, fprs};
    }

    // Runs a point lookup pipeline with discrete channel contention processing
    SimulationResult simulate_with_queue(int queries, double arrival_rate, int num_channels, string mode_name, double retry_p3) {
        vector<double> arrival_times(queries);
        exponential_distribution<double> arrival_dist(arrival_rate);
        
        double current_arrival_time = 0.0;
        for (int i = 0; i < queries; ++i) {
            current_arrival_time += arrival_dist(gen); 
            arrival_times[i] = current_arrival_time;
        }

        priority_queue<double, vector<double>, greater<double>> channel_busy_until;
        for (int i = 0; i < num_channels; ++i) {
            channel_busy_until.push(0.0);
        }

        vector<double> total_latencies;
        total_latencies.reserve(queries);
        double max_completion_time = 0.0;
        double sum_latencies = 0.0; 

        uniform_real_distribution<double> dist(0.0, 1.0);

        for (int i = 0; i < queries; ++i) {
            double req_arrival = arrival_times[i];
            
            // Determine if query is empty and which layer contains the data
            bool is_empty_query = (dist(gen) < empty_query_ratio);
            int actual_layer = -1;
            if (!is_empty_query) {
                double rand_val = dist(gen);
                double cumulative_prob = 0;
                for (int l = 0; l < num_layers; ++l) {
                    cumulative_prob += layers[l].data_prob;
                    if (rand_val <= cumulative_prob) {
                        actual_layer = l;
                        break;
                    }
                }
            }

            // Track the progress cursor of the current read request on timeline
            double current_req_time_cursor = req_arrival;

            // Filter layer by layer and queue I/O dynamically
            for (int l = 0; l < num_layers; ++l) {
                bool is_data_here = (actual_layer == l);
                
                bool bf_says_yes = is_data_here;
                if (!is_data_here) {
                    if (dist(gen) < layers[l].fpr) {
                        bf_says_yes = true;
                        layers[l].fp_count++; 
                    } else {
                        layers[l].intercept_count++; 
                    }
                }

                // If intercepted (bf_says_yes == false), latency is 0 and bypasses queue system
                if (bf_says_yes) {
                    // Physical I/O or false positive triggered; request channel from hardware queue
                    double physical_io_duration = get_read_latency(layers[l].retry_prob, l);
                    
                    double earliest_free_time = channel_busy_until.top();
                    channel_busy_until.pop();

                    // Calculate the start and end timelines for this specific I/O operation
                    double io_start_time = max(current_req_time_cursor, earliest_free_time);
                    double io_completion_time = io_start_time + physical_io_duration;

                    // Update channel availability timestamp
                    channel_busy_until.push(io_completion_time);

                    // Move request timeline cursor forward to the completed I/O event
                    current_req_time_cursor = io_completion_time;

                    // If authentic target data is found, terminate deeper layer processing early
                    if (is_data_here) break;
                }
            }

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

        vector<long long> fps(num_layers), intercepts(num_layers);
        for(int i = 0; i < num_layers; ++i) {
            fps[i] = layers[i].fp_count;
            intercepts[i] = layers[i].intercept_count;
        }

        return {
            mode_name, 
            retry_p3, 
            current_low_ssd_vec[num_layers - 1], 
            calculated_c_l3,
            details.first, 
            details.second, 
            actual_used_bits, 
            mean_latency, 
            get_p(50), 
            get_p(99), 
            get_p(99.9), 
            get_p(99.99),
            throughput,
            fps,
            intercepts
        };
    }
};

int main() {
    vector<SimulationResult> all_results;

    const double retry_min = 0.01;
    const double retry_max = 0.40;
    const double ssd_min = 1.0;
    const double ssd_max = 6.0;

    const int retry_steps = 15;
    const int ssd_steps = 1;
    const int total_random_groups = retry_steps * ssd_steps; 

    cout << "Channels = " << parallel_channels << ", Arrival Rate = " << request_arrival_rate << "\n";
    cout << "Generating " << total_random_groups << " uniform grid (retry, low_ssd) groups...\n\n";

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
            vector<double> current_probs = {0.001, 0.001, 0.001, p3};
            vector<double> current_ssds  = {1.0, 1.0, 1.0, current_low_ssd};
            
            cout << "[" << g << "/" << total_random_groups << "] Simulating grid retry_p3 = " 
                 << fixed << setprecision(4) << p3 << ", low_ssd = " << current_low_ssd << endl;

            // Mode 1: Uniform allocation
            {
                LSMTreeSimulator sim(layers, T, current_probs, current_ssds);
                sim.setup_bloom_filters(1, target_memory_bits);
                all_results.push_back(sim.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 1 (Uniform)", p3));
            }

            // Mode 2: Monkey allocation
            {
                LSMTreeSimulator sim(layers, T, current_probs, current_ssds);
                sim.setup_bloom_filters(2, target_memory_bits);
                all_results.push_back(sim.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 2 (Monkey)", p3));
            }

            // Mode 3: Age-aware Monkey allocation
            {
                LSMTreeSimulator sim(layers, T, current_probs, current_ssds);
                sim.setup_bloom_filters(3, target_memory_bits);
                all_results.push_back(sim.simulate_with_queue(num, request_arrival_rate, parallel_channels, "Mode 3 (Age-aware)", p3));
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