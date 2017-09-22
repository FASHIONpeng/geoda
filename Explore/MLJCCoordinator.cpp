/**
 * GeoDa TM, Copyright (C) 2011-2015 by Luc Anselin - all rights reserved
 *
 * This file is part of GeoDa.
 * 
 * GeoDa is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GeoDa is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <time.h>
#include <boost/math/distributions/normal.hpp> // for normal_distribution
#include <algorithm>
#include <functional>
#include <map>
#include <wx/filename.h>
#include <wx/stopwatch.h>
#include "../DataViewer/TableInterface.h"
#include "../ShapeOperations/Randik.h"
#include "../ShapeOperations/WeightsManState.h"
#include "../VarCalc/WeightsManInterface.h"
#include "../logger.h"
#include "../Project.h"
#include "MLJCCoordinatorObserver.h"
#include "MLJCCoordinator.h"

/*
 Example data: x = {2, 3, 3.2, 5, 8, 7}
 locations = {(10, 10), (20, 10), (40, 10), (15, 20), (30, 20), (30, 30)}
 distances:
	 0      1      2      3      4      5
 0  0.00  10.00  30.00  11.18  22.36  28.28
 1 10.00   0.00  20.00  11.18  14.14  22.36 
 2 30.00  20.00   0.00  26.93  14.14  22.36
 3 11.18  11.18  26.93   0.00  15.00  18.03
 4 22.36  14.14  14.14  15.00   0.00  10.00
 5 28.28  22.36  22.46  18.03  10.00   0.00
 
 W for threshold distance = 15
 0: 1 3
 1: 0 3 4
 2: 4
 3: 0 1 4
 4: 1 2 3 5
 5: 4
 
 W_i = [1, 1, 1, 1, 1]
 
 G_i = (lag_i)/(x_star-x_i)
  = {0.152671756, 0.198412698, 0.32, 0.186781609, 0.225247525, 0.377358491}
 
 n = 6
 ExG = E[G*_i] = 1/(n-1) = 0.2
 ExGstar = E[G_i] = 1/n = 1.6666666
 x_star = 2 + 3 + 3.2 + 5 + 8 + 7 = 28.2
 x_sstar = 2^2 + 3^2 + (3.2)^2 + 5^2 + 8^2 + 7^2 = 161.24
 x_hat = x_star/n = 28.2/6 = 4.7
 s2 = x_sstar/n - (x_hat)^2 = 4.783333
 s = sqrt(s^2) = 2.18708329
 
 x_hat_i = {5.24, 5.04, 5, 4.64, 4.04, 4.24}
 s2_i = (x_sstar-x_i^2)/(n-1) - (x_hat_i)^2
   = {3.9904, 5.0464, 5.2, 5.7184, 3.1264, 4.4704}
 Var[G_i] = (1/(n-1)^2) * (s_i/x_hat_i)^2
  = {0.00581318105, 0.00794658604, 0.00832, 0.0106242568, 0.00766199392,
      0.00994660021  }
 z_i = (G_i - E[G_i]) / sqrt(Var[G_i])
  = {-0.620745338, -0.0178061189, 1.31558703,
     -0.128241714, 0.288434967, 1.77833941}
 
 */


JCWorkerThread::JCWorkerThread(const GalElement* W_, const std::vector<bool>& undefs_, int obs_start_s, int obs_end_s, uint64_t seed_start_s, JCCoordinator* jc_coord_s, wxMutex* worker_list_mutex_s, wxCondition* worker_list_empty_cond_s, std::list<wxThread*> *worker_list_s,int thread_id_s)
: wxThread(),
W(W_),
undefs(undefs_),
obs_start(obs_start_s), obs_end(obs_end_s), seed_start(seed_start_s),
jc_coord(jc_coord_s),
worker_list_mutex(worker_list_mutex_s),
worker_list_empty_cond(worker_list_empty_cond_s),
worker_list(worker_list_s),
thread_id(thread_id_s)
{
}

JCWorkerThread::~JCWorkerThread()
{
}

wxThread::ExitCode JCWorkerThread::Entry()
{
	LOG_MSG(wxString::Format("JCWorkerThread %d started", thread_id));
	// call work for assigned range of observations
	jc_coord->CalcPseudoP_range(W, undefs, obs_start, obs_end, seed_start);
	
	wxMutexLocker lock(*worker_list_mutex);
	// remove ourself from the list
	worker_list->remove(this);
	// if empty, signal on empty condition since only main thread
	// should be waiting on this condition
	if (worker_list->empty()) {
		worker_list_empty_cond->Signal();
	}
	
	return NULL;
}



JCCoordinator::JCCoordinator(boost::uuids::uuid weights_id, Project* project, const std::vector<GdaVarTools::VarInfo>& var_info_s, const std::vector<int>& col_ids, bool row_standardize_weights, bool _is_local_joint_count)
: w_man_state(project->GetWManState()),
w_man_int(project->GetWManInt()),
w_id(weights_id),
num_obs(project->GetNumRecords()),
row_standardize(row_standardize_weights),
permutations(999),
var_info(var_info_s),
data(var_info_s.size()),
data_undef(var_info_s.size()),
last_seed_used(123456789), reuse_last_seed(true),
is_local_joint_count(_is_local_joint_count)
{
    reuse_last_seed = GdaConst::use_gda_user_seed;
    if ( GdaConst::use_gda_user_seed) {
        last_seed_used = GdaConst::gda_user_seed;
    }
	TableInterface* table_int = project->GetTableInt();
	for (int i=0; i<var_info.size(); i++) {
		table_int->GetColData(col_ids[i], data[i]);
        table_int->GetColUndefined(col_ids[i], data_undef[i]);
	}
    
	weight_name = w_man_int->GetLongDispName(w_id);
	SetSignificanceFilter(1);
    
	InitFromVarInfo();
	
	w_man_state->registerObserver(this);
}

JCCoordinator::~JCCoordinator()
{
	w_man_state->removeObserver(this);
	DeallocateVectors();
}

void JCCoordinator::DeallocateVectors()
{
	for (int i=0; i<G_vecs.size(); i++) if (G_vecs[i]) delete [] G_vecs[i];
	G_vecs.clear();

	for (int i=0; i<G_defined_vecs.size(); i++) {
		if (G_defined_vecs[i]) delete [] G_defined_vecs[i];
	}
	G_defined_vecs.clear();

	for (int i=0; i<z_vecs.size(); i++) if (z_vecs[i]) delete [] z_vecs[i];
	z_vecs.clear();

	for (int i=0; i<p_vecs.size(); i++) if (p_vecs[i]) delete [] p_vecs[i];
	p_vecs.clear();
	
	for (int i=0; i<pseudo_p_vecs.size(); i++) {
		if (pseudo_p_vecs[i]) delete [] pseudo_p_vecs[i];
	}
	pseudo_p_vecs.clear();
	
	for (int i=0; i<x_vecs.size(); i++) if (x_vecs[i]) delete [] x_vecs[i];
	x_vecs.clear();
    
	for (int i=0; i<ep_vals.size(); i++) if (ep_vals[i]) delete [] ep_vals[i];
	ep_vals.clear();
    
	for (int i=0; i<num_neighbors_1.size(); i++) if (num_neighbors_1[i]) delete [] num_neighbors_1[i];
	num_neighbors_1.clear();
    
    // clear W_vecs
    for (size_t i=0; i<has_undefined.size(); i++) {
        if (has_undefined[i]) {
            delete Gal_vecs[i];
        }
    }
    Gal_vecs.clear();
    x_undefs.clear();
}

/** allocate based on var_info and num_time_vals **/
void JCCoordinator::AllocateVectors()
{
	int tms = num_time_vals;
	G_vecs.resize(tms);
	G_defined_vecs.resize(tms);
	z_vecs.resize(tms);
	p_vecs.resize(tms);
	pseudo_p_vecs.resize(tms);
	x_vecs.resize(tms);
    
    x_undefs.resize(tms);
    Gal_vecs.resize(tms);
    Gal_vecs_orig.resize(tms);
    
	
	n.resize(tms, 0);
	x_star.resize(tms, 0);
	x_sstar.resize(tms, 0);
	ExG.resize(tms);
	ExGstar.resize(tms);
	mean_x.resize(tms);
	var_x.resize(tms);
	VarGstar.resize(tms);
	sdGstar.resize(tms);
	
	map_valid.resize(tms);
	map_error_message.resize(tms);
	has_isolates.resize(tms);
	has_undefined.resize(tms);
    
    num_neighbors.resize(num_obs);
    num_neighbors_1.resize(tms);
    ep_vals.resize(tms);
    
	for (int i=0; i<tms; i++) {
        num_neighbors_1[i] = new wxInt64[num_obs];
        ep_vals[i] = new double[num_obs];
		G_vecs[i] = new double[num_obs];
		G_defined_vecs[i] = new bool[num_obs];
		for (int j=0; j<num_obs; j++) G_defined_vecs[i][j] = true;
		z_vecs[i] = new double[num_obs];
		p_vecs[i] = new double[num_obs];
		pseudo_p_vecs[i] = new double[num_obs];
		x_vecs[i] = new double[num_obs];
		
		map_valid[i] = true;
		map_error_message[i] = wxEmptyString;
        
        Gal_vecs[i] = NULL;
	}
}

/** We assume only that var_info is initialized correctly.
 ref_var_index, is_any_time_variant, is_any_sync_with_global_time and
 num_time_vals are first updated based on var_info */ 
void JCCoordinator::InitFromVarInfo()
{
	DeallocateVectors();
	
	num_time_vals = 1;
	is_any_time_variant = var_info[0].is_time_variant;
	is_any_sync_with_global_time = false;
	ref_var_index = -1;
	if (var_info[0].is_time_variant &&
		var_info[0].sync_with_global_time) {
		num_time_vals = (var_info[0].time_max - var_info[0].time_min) + 1;
		is_any_sync_with_global_time = true;
		ref_var_index = 0;
	}

	AllocateVectors();
	
    bool has_undef = false;
    
	for (int t=var_info[0].time_min; t<=var_info[0].time_max; t++) {
		int d_t = t - var_info[0].time_min;
        vector<bool> undefs(num_obs);
        for (int i=0; i<num_obs; i++) {
            x_vecs[d_t][i] = data[0][t][i];
            undefs[i] = data_undef[0][t][i];
            G_defined_vecs[d_t][i] = !undefs[i];
            if (undefs[i]) {
                has_undef = true;
            }
        }
        x_undefs[d_t] = undefs;
        has_undefined[d_t] = has_undef;
	}
    
	for (int t=0; t<num_time_vals; t++) {
        GalElement* W  = NULL;
        if (Gal_vecs.empty() || Gal_vecs[t] == NULL) {
            // local weights copy: leak?
            GalWeight* gw = NULL;
            if ( has_undefined[t] ) {
                gw = new GalWeight(*w_man_int->GetGal(w_id));
                gw->Update(x_undefs[t]);
            } else {
                gw = w_man_int->GetGal(w_id);
            }
            W = gw->gal ;
            Gal_vecs[t] = gw;
            Gal_vecs_orig[t] = w_man_int->GetGal(w_id);
        }
        
		x = x_vecs[t];
       
        for (int i=0; i<num_obs; i++) {
            if ( W[i].Size() > 0 ) {
                n[t]++;
                x_star[t] += x[i];
                x_sstar[t] += x[i] * x[i];
            }
        }
        
        num_obs_1s = 0;
        num_obs_0s = 0;
        
        // count neighbors and neighors with 1
        for (int i=0; i<num_obs; i++) {
            num_neighbors[i] = W[i].Size();
            num_neighbors_1[t][i] = 0;
            for (int j=0; j< W[i].Size(); j++) {
                if (x[W[i][j]] == 1) {
                    num_neighbors_1[t][i] += 1;
                }
            }
            if (x[i] ==1)  num_obs_1s +=1;
            else num_obs_0s += 1;
        }
	}
	
	CalcGs();
	CalcPseudoP();
}

/** Update Secondary Attributes based on Primary Attributes.
 Update num_time_vals and ref_var_index based on Secondary Attributes. */
void JCCoordinator::VarInfoAttributeChange()
{
	GdaVarTools::UpdateVarInfoSecondaryAttribs(var_info);
	
	is_any_time_variant = false;
	is_any_sync_with_global_time = false;
	for (int i=0; i<var_info.size(); i++) {
		if (var_info[i].is_time_variant) is_any_time_variant = true;
		if (var_info[i].sync_with_global_time) {
			is_any_sync_with_global_time = true;
		}
	}
	ref_var_index = -1;
	num_time_vals = 1;
	for (int i=0; i<var_info.size() && ref_var_index == -1; i++) {
		if (var_info[i].is_ref_variable) ref_var_index = i;
	}
	if (ref_var_index != -1) {
		num_time_vals = (var_info[ref_var_index].time_max -
						 var_info[ref_var_index].time_min) + 1;
	}
}

/** The category vector c_val will be filled based on the current
 significance filter and significance values corresponding to specified
 canvas_time.  */
void JCCoordinator::FillClusterCats(int canvas_time,
									   bool is_gi,
                                       bool is_perm,
									   std::vector<wxInt64>& c_val)
{
	int t = canvas_time;
	double* p_val = pseudo_p_vecs[t];
    double* z_val = z_vecs[t];
	
    const GalElement* W = Gal_vecs[t]->gal;
    
	c_val.resize(num_obs);
	for (int i=0; i<num_obs; i++) {
        if (!G_defined_vecs[t][i]) {
            c_val[i] = 4; // undefined
            
        } else if (W[i].Size() == 0) {
			c_val[i] = 3; // isolate
            
		} else if (p_val[i] <= significance_cutoff) {
            if (x_vecs[t][i] == 1 && z_val[i] > 0)
                c_val[i] = 1;
            else
                c_val[i] = 0;
        } else {
			c_val[i] = 0; // not significant
		}
	}
}


void JCCoordinator::CalcGs()
{
	using boost::math::normal; // typedef provides default type is double.
	// Construct a standard normal distribution std_norm_dist
	normal std_norm_dist; // default mean = zero, and s.d. = unity
	
	for (int t=0; t<num_time_vals; t++) {
		G = G_vecs[t];
		G_defined = G_defined_vecs[t];
		z = z_vecs[t];
		p = p_vecs[t];
		pseudo_p = pseudo_p_vecs[t];
		x = x_vecs[t];
		
		has_undefined[t] = false;
		has_isolates[t] = false;
        
        const GalElement* W = Gal_vecs[t]->gal;

		double n_expr = sqrt((n[t]-1)*(n[t]-1)*(n[t]-2));
		for (long i=0; i<num_obs; i++) {
            if (x_undefs[t][i]) {
                G_defined[i] = false;
                has_undefined[t] = true;
                continue;
            }
            
			const GalElement& elm_i = W[i];
			if ( elm_i.Size() > 0 ) {
				double lag = 0;
				bool self_neighbor = false;
				for (size_t j=0, sz=W[i].Size(); j<sz; j++) {
					if (elm_i[j] != i) {
						lag += x[elm_i[j]];
					} else {
						self_neighbor = true;
					}
				}
				double Wi = self_neighbor ? W[i].Size()-1 : W[i].Size();
				double xd_i = x_star[t] - x[i];
				if (xd_i != 0) {
					G[i] = lag / xd_i;
				} else {
					G_defined[i] = false;
				}
				double x_hat_i = xd_i * ExG[t]; // (x_star - x[i])/(n-1)

				double ExGi = Wi/(n[t]-1);
				// location-specific variance
				double ss_i = ((x_sstar[t] - x[i]*x[i])/(n[t]-1)
							   - x_hat_i*x_hat_i);
				double sdG_i = sqrt(Wi*(n[t]-1-Wi)*ss_i)/(n_expr * x_hat_i);
				
				// compute z and one-sided p-val from standard-normal table
				if (G_defined[i]) {
					z[i] = (G[i] - ExGi)/sdG_i;
					if (z[i] >= 0) {
						p[i] = 1.0-cdf(std_norm_dist, z[i]);
					} else {
						p[i] = cdf(std_norm_dist, z[i]);
					}
				} else {
					has_undefined[t] = true;
				}
			} else {
				has_isolates[t] = true;
			}
		}
	
		if (x_star[t] == 0) {
            for (long i=0; i<num_obs; i++) {
                G_defined[i] = false;
            }
			has_undefined[t] = true;
			break;
		}
	}
}

void JCCoordinator::CalcPseudoP()
{
	LOG_MSG("Entering JCCoordinator::CalcPseudoP");
	wxStopWatch sw;
	int nCPUs = wxThread::GetCPUCount();
	
	// To ensure thread safety, only work on one time slice of data
	// at a time.  For each time period t:
	// 1. copy data for time period t into data1 and data2 arrays
	// 2. Perform multi-threaded computation
	// 3. copy results into results array
	
	for (int t=0; t<num_time_vals; t++) {
        std::vector<bool>& undefs = x_undefs[t];
		G = G_vecs[t];
		G_defined = G_defined_vecs[t];
		z = z_vecs[t];
		p = p_vecs[t];
		pseudo_p = pseudo_p_vecs[t];
		x = x_vecs[t];
		x_star_t = x_star[t];
        nn_1_t = num_neighbors_1[t];
        e_p = ep_vals[t];
		if (nCPUs <= 1) {
			if (!reuse_last_seed) last_seed_used = time(0);
			CalcPseudoP_range(Gal_vecs[t]->gal, undefs, 0, num_obs-1, last_seed_used);
		} else {
			CalcPseudoP_threaded(Gal_vecs[t]->gal, undefs);
		}
	}
	LOG_MSG("Exiting JCCoordinator::CalcPseudoP");
}

void JCCoordinator::CalcPseudoP_threaded(const GalElement* W, const std::vector<bool>& undefs)
{
	LOG_MSG("Entering JCCoordinator::CalcPseudoP_threaded");
	int nCPUs = wxThread::GetCPUCount();
	
	// mutext protects access to the worker_list
    wxMutex worker_list_mutex;
	// signals that worker_list is empty
	wxCondition worker_list_empty_cond(worker_list_mutex);
	worker_list_mutex.Lock(); // mutex should be initially locked
	
    // List of all the threads currently alive.  As soon as the thread
	// terminates, it removes itself from the list.
	std::list<wxThread*> worker_list;
	
	// divide up work according to number of observations
	// and number of CPUs
	int work_chunk = num_obs / nCPUs;
	int obs_start = 0;
	int obs_end = obs_start + work_chunk;
	
	bool is_thread_error = false;
	int quotient = num_obs / nCPUs;
	int remainder = num_obs % nCPUs;
	int tot_threads = (quotient > 0) ? nCPUs : remainder;
	
	if (!reuse_last_seed) last_seed_used = time(0);
	for (int i=0; i<tot_threads && !is_thread_error; i++) {
		int a=0;
		int b=0;
		if (i < remainder) {
			a = i*(quotient+1);
			b = a+quotient;
		} else {
			a = remainder*(quotient+1) + (i-remainder)*quotient;
			b = a+quotient-1;
		}
		uint64_t seed_start = last_seed_used+a;
		uint64_t seed_end = seed_start + ((uint64_t) (b-a));
		int thread_id = i+1;
		wxString msg;
		msg << "thread " << thread_id << ": " << a << "->" << b;
		msg << ", seed: " << seed_start << "->" << seed_end;
		
		JCWorkerThread* thread =
			new JCWorkerThread(W, undefs, a, b, seed_start, this,
								  &worker_list_mutex,
								  &worker_list_empty_cond,
								  &worker_list, thread_id);
		if ( thread->Create() != wxTHREAD_NO_ERROR ) {
			delete thread;
			is_thread_error = true;
		} else {
			worker_list.push_front(thread);
		}
	}
	if (is_thread_error) {
		// fall back to single thread calculation mode
		CalcPseudoP_range(W, undefs, 0, num_obs-1, last_seed_used);
	} else {
		std::list<wxThread*>::iterator it;
		for (it = worker_list.begin(); it != worker_list.end(); it++) {
			(*it)->Run();
		}
		
		while (!worker_list.empty()) {
			// wait until thread_list might be empty
			worker_list_empty_cond.Wait();
			// We have been woken up. If this was not a false
			// alarm (spurious signal), the loop will exit.
		}
	}
	
	LOG_MSG("Exiting JCCoordinator::CalcPseudoP_threaded");
}

/** In the code that computes Gi and Gi*, we specifically checked for 
 self-neighbors and handled the situation appropriately.  For the
 permutation code, we will disallow self-neighbors. */
void JCCoordinator::CalcPseudoP_range(const GalElement* W, const std::vector<bool>& undefs, int obs_start, int obs_end, uint64_t seed_start)
{
	GeoDaSet workPermutation(num_obs);
    
	int max_rand = num_obs-1;
    
	for (long i=obs_start; i<=obs_end; i++) {
        if (undefs[i]) continue;

		const int numNeighsI = W[i].Size();
		const double numNeighsD = W[i].Size();
        
        //only compute for non-isolates
		if (numNeighsI > 0 && G_defined[i]) {
            // know != 0 since G_defined[i] true
			double xd_i = x_star_t - x[i];
			
			int countGLarger = 0;
			double permutedG = 0;
            
			for (int perm=0; perm < permutations; perm++) {
				int rand = 0;
				while (rand < numNeighsI) {
					// computing 'perfect' permutation of given size
                    double rng_val = Gda::ThomasWangHashDouble(seed_start++) * max_rand;
                    // round is needed to fix issue
                    //https://github.com/GeoDaCenter/geoda/issues/488
                    int newRandom = (int) (rng_val < 0.0 ? ceil(rng_val - 0.5) : floor(rng_val + 0.5));
                    
					if (newRandom != i &&
                        !workPermutation.Belongs(newRandom) &&
                        undefs[newRandom] == false)
					{
						workPermutation.Push(newRandom);
						rand++;
					}
				}
				
				double lag_i=0;
				// use permutation to compute the lags
				for (int j=0; j<numNeighsI; j++) {
					lag_i += x[workPermutation.Pop()];
					//lag_j += y[workPermutation.Pop()];
				}
		
                // binary weights
                permutedG = lag_i / xd_i;
				if (permutedG >= G[i]) countGLarger++;
			}
			// pick the smallest
			if (permutations-countGLarger < countGLarger) { 
				countGLarger=permutations-countGLarger;
			}
			pseudo_p[i] = (countGLarger + 1.0)/(permutations+1.0);
			
            // compute exact probability
            if (is_local_joint_count) {
                int nn = num_neighbors[i];
                int n_1s = nn_1_t[i];
                int n_0s = nn - n_1s;
                
                double mm_all = Gda::nChoosek(num_obs-1, nn);
                double mm_1s = Gda::nChoosek(num_obs_1s-1, n_1s);
                double mm_0s = Gda::nChoosek(num_obs_0s, n_0s);
                                        
                double hg = (mm_1s * mm_0s) / mm_all;
                e_p[i] = hg;
            }
		}
	}
}

void JCCoordinator::SetSignificanceFilter(int filter_id)
{
    if (filter_id == -1) {
        // user input cutoff
        significance_filter = filter_id;
        return;
    }
	// 0: >0.05 1: 0.05, 2: 0.01, 3: 0.001, 4: 0.0001
	if (filter_id < 1 || filter_id > 4) return;
	significance_filter = filter_id;
	if (filter_id == 1) significance_cutoff = 0.05;
	if (filter_id == 2) significance_cutoff = 0.01;
	if (filter_id == 3) significance_cutoff = 0.001;
	if (filter_id == 4) significance_cutoff = 0.0001;
}

void JCCoordinator::update(WeightsManState* o)
{
	weight_name = w_man_int->GetLongDispName(w_id);
}

int JCCoordinator::numMustCloseToRemove(boost::uuids::uuid id) const
{
	return id == w_id ? observers.size() : 0;
}

void JCCoordinator::closeObserver(boost::uuids::uuid id)
{
	if (numMustCloseToRemove(id) == 0) return;
	std::list<JCCoordinatorObserver*> observers_cpy = observers;
	for (std::list<JCCoordinatorObserver*>::iterator i=observers_cpy.begin();
		 i != observers_cpy.end(); ++i) {
		if (*i != 0) {
			(*i)->closeObserver(this);
		}
	}
}

void JCCoordinator::registerObserver(JCCoordinatorObserver* o)
{
	observers.push_front(o);
}

void JCCoordinator::removeObserver(JCCoordinatorObserver* o)
{
    observers.remove(o);
    LOG(observers.size());
    if (observers.size() == 0) {
        delete this;
    }
}

void JCCoordinator::notifyObservers()
{
    for (std::list<JCCoordinatorObserver*>::iterator  it=observers.begin();
         it != observers.end(); ++it) {
        (*it)->update(this);
    }
}
