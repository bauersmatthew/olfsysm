#include "olfsysm.hpp"

#include <math.h>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <iostream>
#include <functional>
#include <sstream>

Logger::Logger() {}
Logger::Logger(Logger const&) {
    throw std::runtime_error("Can't copy Logger instances.");
}
void Logger::operator()(std::string const& msg) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!fout) return;
    fout << msg << std::endl;
}
void Logger::operator()() {
    this->operator()("");
}
void Logger::redirect(std::string const& path) {
    std::lock_guard<std::mutex> lock(mtx);
    fout.close();
    fout.open(path, std::ofstream::out | std::ofstream::app);
}
void Logger::disable() {
    std::lock_guard<std::mutex> lock(mtx);
    fout.close();
}

/* Concatenate all the given arguments, which can be of any type, into one
 * string. No separator is placed between the arguments! */
template<class... Args>
std::string cat(Args&&... args) {
    std::stringstream ss;
    (ss << ... << std::forward<Args>(args));
    return ss.str();
}

/* For random number generation. */
thread_local std::random_device g_randdev;
thread_local std::mt19937 g_randgen{g_randdev()};

ModelParams const DEFAULT_PARAMS = []() {
    ModelParams p;

    p.time.pre_start  = -2.0;
    p.time.start      = -0.5;
    p.time.end        = 0.75;
    p.time.stim.start = 0.0;
    p.time.stim.end   = 0.5;
    p.time.dt         = 0.5e-3;

    p.orn.taum             = 0.01;
    p.orn.n_physical_gloms = 51;

    p.ln.taum   = 0.01;
    p.ln.tauGA  = 0.1;
    p.ln.tauGB  = 0.4;
    p.ln.thr    = 1.0;
    p.ln.inhsc  = 500.0;
    p.ln.inhadd = 200.0;

    p.pn.taum       = 0.01;
    p.pn.offset     = 2.9410;
    p.pn.tanhsc     = 5.3395;
    p.pn.inhsc      = 368.6631;
    p.pn.inhadd     = 31.4088;
    p.pn.noise.mean = 0.0;
    p.pn.noise.sd   = 0.0;

    p.kc.N                    = 2000;
    p.kc.nclaws               = 6;
    p.kc.uniform_pns          = false;
    p.kc.enable_apl           = true;
    p.kc.fixed_thr            = 0;
    p.kc.use_fixed_thr        = false;
    p.kc.use_homeostatic_thrs = true;
    p.kc.sp_target            = 0.1;
    p.kc.sp_acc               = 0.1;
    p.kc.sp_lr_coeff          = 10.0;
    p.kc.max_iters            = 10;
    p.kc.taum                 = 0.01;
    p.kc.apl_taum             = 0.05;
    p.kc.tau_apl2kc           = 0.01;

    return p;
}();

/* (utility) Split a string by commas, and fill vec with the segments.
 * vec must be sized correctly! */
void split_regular_csv(std::string const& str, std::vector<std::string>& vec);

/* The exponential ('e') part of the smoothts MATLAB function included in the
 * Kennedy source.
 * Instead of returning the smoothed matrix, it smooths it in-place. */
void smoothts_exp(Matrix& vin, double wsize);

/* Fill out with numbers generated by rng. */
void add_randomly(std::function<double()> rng, Matrix& out);

/* Randomly generate the wPNKC connectivity matrix. Glom choice is WEIGHTED by
 * HC_GLOM_CXN_DISTRIB (above). */
void build_wPNKC_weighted(ModelParams const& p, RunVars& rv);
/* Randomly generate the wPNKC connectivity matrix. Glom choice is UNIFORMLY
 * weighted. */
void build_wPNKC_uniform(ModelParams const& p, RunVars& rv);

/* Sample spontaneous PN output from odor 0. */
Column sample_PN_spont(ModelParams const& p, RunVars const& rv);

/* Decide a KC threshold column from KC membrane voltage data. */
Column choose_KC_thresh(
        ModelParams const& p, Matrix& KCpks, Column const& spont_in);

/* Remove all columns <step in timecourse.*/
void remove_before(unsigned step, Matrix& timecourse);
/* Remove all pretime columns in all timecourses in r. */
void remove_all_pretime(ModelParams const& p, RunVars& r);

/*******************************************************************************
********************************************************************************
*********************                                      *********************
*********************            IMPLEMENTATIONS           *********************
*********************                                      *********************
********************************************************************************
*******************************************************************************/
inline unsigned get_ngloms(ModelParams const& mp) {
    return mp.orn.data.delta.rows();
}
inline unsigned get_nodors(ModelParams const& mp) {
    return mp.orn.data.delta.cols();
}

ModelParams::Time::Time() : stim(*this) {
}
ModelParams::Time::Time(Time const& o) : stim(*this) {
    pre_start  = o.pre_start;
    start      = o.start;
    end        = o.end;
    stim.start = o.stim.start;
    stim.end   = o.stim.end;
    dt         = o.dt;
}
ModelParams::Time::Stim::Stim(ModelParams::Time& o) : _owner(o) {
}
unsigned ModelParams::Time::Stim::start_step() const {
    return (start - _owner.pre_start)/_owner.dt;
}
unsigned ModelParams::Time::Stim::end_step() const {
    return (end - _owner.pre_start)/_owner.dt;
}
Row ModelParams::Time::Stim::row_all() const {
    Row ret(1, _owner.steps_all());
    ret.setZero();
    for (unsigned i = start_step(); i < end_step(); i++) {
        ret(i) = 1.0;
    }
    return ret;
}
unsigned ModelParams::Time::start_step() const {
    return (start-pre_start)/dt;
}
unsigned ModelParams::Time::steps_all() const {
    return (end-pre_start)/dt;
}
unsigned ModelParams::Time::steps() const {
    return (end-start)/dt;
}
Row ModelParams::Time::row_all() const {
    Row ret(1, steps_all());
    ret.setOnes();
    return ret;
}

RunVars::RunVars(ModelParams const& p) : orn(p), ln(p), pn(p), kc(p) {
}
RunVars::ORN::ORN(ModelParams const& p) :
    sims(get_nodors(p), Matrix(get_ngloms(p), p.time.steps_all())) {
}
RunVars::LN::LN(ModelParams const& p) :
    inhA{std::vector<Vector>(get_nodors(p), Row(1, p.time.steps_all()))},
    inhB{std::vector<Vector>(get_nodors(p), Row(1, p.time.steps_all()))} {
}
RunVars::PN::PN(ModelParams const& p) :
    sims(get_nodors(p), Matrix(get_ngloms(p), p.time.steps_all())) {
}
RunVars::KC::KC(ModelParams const& p) :
    wPNKC(p.kc.N, get_ngloms(p)),
    wAPLKC(p.kc.N, 1),
    wKCAPL(1, p.kc.N),
    thr(p.kc.N, 1),
    responses(p.kc.N, get_nodors(p)),
    spike_counts(p.kc.N, get_nodors(p)),
    tuning_iters(0) {
}

void split_regular_csv(std::string const& str, std::vector<std::string>& vec) {
    int seg = 0;
    std::string growing;
    for (char ch : str) {
        if (ch == ',') {
            vec[seg++] = growing;
            growing = "";
        }
        else {
            growing += ch;
        }
    }
    vec[vec.size()-1] = growing;
}

/* Helper function for load_hc_data(). */
void load_hc_data_line(
        std::string const& line, std::vector<std::string>& segs,
        Matrix& out, unsigned col) {
    unsigned const N_HC_GLOMS  = 23;
    split_regular_csv(line, segs);
    unsigned g8fix = 0; // decrement the column ID of odors after glom 8
    for (unsigned glom = 0; glom < N_HC_GLOMS+1; glom++) {
        /* Ignore the 8th glom column (Kennedy does this). */
        if (glom == 7) {
            g8fix = 1;
            continue;
        }

        out(glom-g8fix, col) = std::stod(segs[glom+2]);
    }
}
void load_hc_data(ModelParams& p, std::string const& fpath) {
    unsigned const N_HC_ODORS  = 110; // all original HC odors
    unsigned const N_HC_GLOMS  = 23;  // all good HC gloms
    unsigned const N_ODORS_ALL = 186; // all odors in Kennedy's HC data file

    p.orn.data.delta.resize(N_HC_GLOMS, N_HC_ODORS);
    p.orn.data.spont.resize(N_HC_GLOMS, 1);

    std::ifstream fin(fpath);
    std::string line;

    /* Discard the first two (header) lines. */
    std::getline(fin, line);
    std::getline(fin, line);

    /* Read the rest of the lines (except the last one). */
    std::vector<std::string> segs(N_HC_GLOMS+2+1); // 2 ID cols, 1 bad glom col
    for (unsigned odor = 0; odor < N_ODORS_ALL; odor++) {
        std::getline(fin, line);

        /* We need to read to the end of the file, but we aren't interested in
         * any of the the non-HC odors. */
        if (odor >= N_HC_ODORS) continue;

        /* Parse and store data. */
        load_hc_data_line(line, segs, p.orn.data.delta, odor);
    }

    /* Load the spontaneous rates line. */
    std::getline(fin, line);
    load_hc_data_line(line, segs, p.orn.data.spont, 0);

    /* Load connectivity distribution data. */
    p.kc.cxn_distrib.resize(1, N_HC_GLOMS);
    /* Data presumably taken from some real measurements.
     * Taken from Kennedy source. */
    p.kc.cxn_distrib <<
        2.0, 24.0, 4.0, 30.0, 33.0, 8.0, 0.0,
        // no #8!
        29.0, 6.0, 2.0, 4.0, 21.0, 18.0, 4.0,
        12.0, 21.0, 10.0, 27.0, 4.0, 26.0, 7.0,
        26.0, 24.0;
}

void smoothts_exp(Matrix& vin, double wsize) {
    double extarg = wsize;
    if (wsize > 1.0) {
        extarg = 2.0/(wsize+1.0);
    }
    for (int i = 1; i < vin.cols(); i++) {
        vin.col(i) = extarg*vin.col(i) + (1-extarg)*vin.col(i-1);
    }
}

void add_randomly(std::function<double()> rng, Matrix& out) {
    for (unsigned i = 0; i < out.rows(); i++) {
        for (unsigned j = 0; j < out.cols(); j++) {
            out(i, j) += rng();
        }
    }
}

void build_wPNKC_from_cxnd(Matrix& w, unsigned nc, Row const& cxnd) {
    w.setZero();
    std::vector<double> flat(cxnd.size());
    for (unsigned i = 0; i < cxnd.size(); i++) {
        flat[i] = cxnd(0, i);
    }
    std::discrete_distribution<int> dd(flat.begin(), flat.end());
    for (unsigned kc = 0; kc < w.rows(); kc++) {
        for (unsigned claw = 0; claw < nc; claw++) {
            w(kc, dd(g_randgen)) += 1.0;
        }
    }
}
void build_wPNKC(ModelParams const& p, RunVars& rv) {
    if (p.kc.uniform_pns) {
        rv.log("building UNIFORM connectivity matrix");
        Row cxnd(1, get_ngloms(p));
        cxnd.setOnes();
        build_wPNKC_from_cxnd(rv.kc.wPNKC, p.kc.nclaws, cxnd);
    }
    else {
        rv.log("building WEIGHTED connectivity matrix");
        build_wPNKC_from_cxnd(rv.kc.wPNKC, p.kc.nclaws, p.kc.cxn_distrib);
    }
}
Column sample_PN_spont(ModelParams const& p, RunVars const& rv) {
    /* Sample from halfway between time start and stim start to stim start. */
    unsigned sp_t1 =
        p.time.start_step()
        + unsigned((p.time.stim.start-p.time.start)/(2*p.time.dt));
    unsigned sp_t2 =
        p.time.start_step()
        + unsigned((p.time.stim.start-p.time.start)/(p.time.dt));
    return rv.pn.sims[0].block(0,sp_t1,get_ngloms(p),sp_t2-sp_t1).rowwise().mean();
}
Column choose_KC_thresh_uniform(
        ModelParams const& p, Matrix& KCpks, Column const& spont_in) {
    unsigned tlist_sz = KCpks.cols();
    KCpks.resize(1, KCpks.size());                     // flatten
    std::sort(KCpks.data(), KCpks.data()+KCpks.size(),
            [](double a, double b){return a>b;});      // dec. order
    double thr_const = KCpks(std::min(
                int(p.kc.sp_target*2.0*double(p.kc.N*tlist_sz)),
                int(p.kc.N*tlist_sz)-1));
    return thr_const + spont_in.array()*2.0;
}
Column choose_KC_thresh_homeostatic(
        ModelParams const& p, Matrix& KCpks, Column const& spont_in) {
    /* Basically do the same procedure as the uniform algorithm, but do it for
     * each KC (row) separately instead of all together. 
     * To sort each row in place, we first flatten the entire list, and then
     * sort portions of it in place. This is an unfortunate consequence of the
     * lack of stl iterators in Eigen <=3.4. */
    Column thr = 2.0*spont_in;
    unsigned cols = KCpks.cols();
    unsigned wanted = p.kc.sp_target*2.0*double(cols);
    KCpks.transposeInPlace();
    KCpks.resize(1, KCpks.size());
    /* Choose a threshold for each KC by inspecting its sorted responses. */
    for (unsigned i = 0; i < p.kc.N; i++) {
        unsigned offset = i*cols;
        std::sort(KCpks.data()+offset, KCpks.data()+offset+cols,
                std::greater<double>());
        thr(i) += KCpks(offset+wanted);
    }
    return thr;
}
void fit_sparseness(ModelParams const& p, RunVars& rv) {
    rv.log("fitting sparseness");

    std::vector<unsigned> tlist = p.kc.tune_from;
    if (!tlist.size()) {
        for (unsigned i = 0; i < get_nodors(p); i++) tlist.push_back(i);
    }

    /* Set starting values for the things we'll tune. */
    rv.kc.wAPLKC.setZero();
    rv.kc.wKCAPL.setConstant(1.0/float(p.kc.N));
    if (!p.kc.use_fixed_thr) {
        rv.kc.thr.setConstant(1e5); // higher than will ever be reached
    }
    else {
        rv.log(cat("using FIXED threshold: ", p.kc.fixed_thr));
        rv.kc.thr.setConstant(p.kc.fixed_thr);
    }

    /* Calculate spontaneous input to KCs. */
    Column spont_in = rv.kc.wPNKC * sample_PN_spont(p, rv);

    /* Used for measuring KC voltage; defined here to make it shared across all
     * threads.*/
    Matrix KCpks(p.kc.N, tlist.size()); KCpks.setZero();

    /* Used to store odor response data during APL tuning. */
    Matrix KCmean_st(p.kc.N, 1+((tlist.size()-1)/3));
    /* Used to store the current sparsity.
     * Initially set to the below value because, given default model
     * parameters, it causes tuning to complete in just one iteration. */
    double sp = 0.0789;
    /* Used to count number of times looped; the 'learning rate' is decreased
     * as 1/sqrt(count) with each iteration. */
    rv.kc.tuning_iters = 0;

    /* Break up into threads. */
#pragma omp parallel
    { 
        /* Output matrices for the KC simulation. */
        Matrix Vm(p.kc.N, p.time.steps_all());
        Matrix spikes(p.kc.N, p.time.steps_all());

        if (!p.kc.use_fixed_thr) {
#pragma omp single
            {
                rv.log("choosing thresholds from spontaneous input");
            }

            /* Measure voltages achieved by the KCs, and choose a threshold
             * based on that. */
#pragma omp for
            for (unsigned i = 0; i < tlist.size(); i++) {
                sim_KC_layer(p, rv, rv.pn.sims[tlist[i]], Vm, spikes);
                KCpks.col(i) = Vm.rowwise().maxCoeff() - spont_in*2.0;
            }

#pragma omp single
            {
                /* Finish picking thresholds. */
                if (p.kc.use_homeostatic_thrs) {
                    rv.kc.thr = choose_KC_thresh_homeostatic(p, KCpks, spont_in);
                }
                else {
                    rv.kc.thr = choose_KC_thresh_uniform(p, KCpks, spont_in);
                }
            }
        }

        /* Enter this region only if APL use is enabled; if disabled, just exit
         * (at this point APL->KC weights are set to 0). */
        if (p.kc.enable_apl) {
#pragma omp single
        {
            rv.log(cat("APL enabled; tuning begin (",
                        "target=", p.kc.sp_target,
                        " acc=", p.kc.sp_acc,
                        ")"));

            rv.kc.tuning_iters = 1;
            /* Starting values for to-be-tuned APL<->KC weights. */
            rv.kc.wAPLKC.setConstant(
                    2*ceil(-log(p.kc.sp_target)));
            rv.kc.wKCAPL.setConstant(
                    2*ceil(-log(p.kc.sp_target))/double(p.kc.N));
        }

        /* Continue tuning until we reach the desired sparsity. */
        do {
            rv.log(cat("** t", omp_get_thread_num(), " @ top"));
#pragma omp barrier

#pragma omp single
            {
                /* Modify the APL<->KC weights in order to move in the
                 * direction of the target sparsity. */
                double lr = p.kc.sp_lr_coeff/sqrt(double(rv.kc.tuning_iters));
                double delta = (sp-p.kc.sp_target)*lr/p.kc.sp_target;
                rv.kc.wAPLKC.array() += delta;
                rv.kc.wKCAPL.array() += delta/double(p.kc.N);

                /* If we learn too fast in the negative direction we could end
                 * up with negative weights. */
                if (delta < 0.0) {
                    rv.kc.wAPLKC = (rv.kc.wAPLKC.array() < 0.0).select(
                            0.0, rv.kc.wAPLKC);
                    rv.kc.wKCAPL = (rv.kc.wKCAPL.array() < 0.0).select(
                            0.0, rv.kc.wKCAPL);
                }

                rv.log(cat( "* i=", rv.kc.tuning_iters,
                            ", sp=", sp, 
                            ", d=", delta,
                            ", lr=", lr));

                rv.kc.tuning_iters++;
            }

            rv.log(cat("** t", omp_get_thread_num(), " @ before testing"));
            /* Run through a bunch of odors to test sparsity. */
#pragma omp for
            for (unsigned i = 0; i < tlist.size(); i+=3) {
                sim_KC_layer(p, rv, rv.pn.sims[tlist[i]], Vm, spikes);
                KCmean_st.col(i/3) = spikes.rowwise().sum();
            }
            rv.log(cat("** t", omp_get_thread_num(), " @ after testing"));

#pragma omp single
            {
                KCmean_st = (KCmean_st.array() > 0.0).select(1.0, KCmean_st);
                sp = KCmean_st.mean();
            }

            rv.log(cat("** t", omp_get_thread_num(), " @ before bottom cond [",
                        "sp=", sp,
                        ", i=", rv.kc.tuning_iters,
                        ", tgt=", p.kc.sp_target,
                        ", acc=", p.kc.sp_acc,
                        ", I=", p.kc.max_iters,
                        "]"));
        } while ((abs(sp-p.kc.sp_target)>(p.kc.sp_acc*p.kc.sp_target))
                && (rv.kc.tuning_iters <= p.kc.max_iters));
        rv.log(cat("** t", omp_get_thread_num(), " @ exit"));
#pragma omp barrier
#pragma omp single
        {
            rv.kc.tuning_iters--;
        }
    }}
    rv.log("done fitting sparseness");
}

void sim_ORN_layer(
        ModelParams const& p, RunVars const& rv,
        int odorid,
        Matrix& orn_t) {
    /* Initialize with spontaneous activity. */
    orn_t = p.orn.data.spont*p.time.row_all();

    /* "Odor input to ORNs" (Kennedy comment)
     * Smoothed timeseries of spont...odor rate...spont */
    Matrix odor = orn_t + p.orn.data.delta.col(odorid)*p.time.stim.row_all();
    smoothts_exp(odor, 0.02/p.time.dt); // where does 0.02 come from!?

    double mul = p.time.dt/p.orn.taum;
    for (unsigned t = 1; t < p.time.steps_all(); t++) {
        orn_t.col(t) = orn_t.col(t-1)*(1.0-mul) + odor.col(t)*mul;
    }
}
void sim_LN_layer(
        ModelParams const& p,
        Matrix const& orn_t,
        Row& inhA, Row& inhB) {
    Row potential(1, p.time.steps_all()); potential.setConstant(300.0);
    Row response(1, p.time.steps_all());  response.setOnes();
    inhA.setConstant(50.0);
    inhB.setConstant(50.0);
    double inh_LN = 0.0;

    double dinhAdt, dinhBdt, dLNdt;
    double scaling = double(get_ngloms(p))/double(p.orn.n_physical_gloms);
    for (unsigned t = 1; t < p.time.steps_all(); t++) {
        dinhAdt = -inhA(t-1) + response(t-1);
        dinhBdt = -inhB(t-1) + response(t-1);
        dLNdt =
            -potential(t-1)
            +pow(orn_t.col(t-1).mean()*scaling, 3.0)/scaling/2.0*inh_LN;
        inhA(t) = inhA(t-1) + dinhAdt*p.time.dt/p.ln.tauGA;
        inhB(t) = inhB(t-1) + dinhBdt*p.time.dt/p.ln.tauGB;
        inh_LN = p.ln.inhsc/(p.ln.inhadd+inhA(t));
        potential(t) = potential(t-1) + dLNdt*p.time.dt/p.ln.taum;
        //response(t) = potential(t) > lnp.thr ? potential(t)-lnp.thr : 0.0;
        response(t) = (potential(t)-p.ln.thr)*double(potential(t)>p.ln.thr);
    }
}
void sim_PN_layer(
        ModelParams const& p, RunVars const& rv,
        Matrix const& orn_t, Row const& inhA, Row const& inhB, 
        Matrix& pn_t) {                                        
    std::normal_distribution<double> noise(p.pn.noise.mean, p.pn.noise.sd);

    Column spont  = p.orn.data.spont*p.pn.inhsc/(p.orn.data.spont.sum()+p.pn.inhadd);
    pn_t          = p.orn.data.spont*p.time.row_all();
    double inh_PN = 0.0;

    Column orn_delta;
    Column dPNdt;
    for (unsigned t = 1; t < p.time.steps_all(); t++) {
        orn_delta = orn_t.col(t-1)-p.orn.data.spont;
        dPNdt = -pn_t.col(t-1) + spont;
        dPNdt += 
            200.0*((orn_delta.array()+p.pn.offset)*p.pn.tanhsc/200.0*inh_PN).matrix().unaryExpr<double(*)(double)>(&tanh);
        add_randomly([&noise](){return noise(g_randgen);}, dPNdt);

        inh_PN = p.pn.inhsc/(p.pn.inhadd+0.25*inhA(t)+0.75*inhB(t));
        pn_t.col(t) = pn_t.col(t-1) + dPNdt*p.time.dt/p.pn.taum;
        pn_t.col(t) = (0.0 < pn_t.col(t).array()).select(pn_t.col(t), 0.0);
    }
}
void sim_KC_layer(
        ModelParams const& p, RunVars const& rv,
        Matrix const& pn_t,
        Matrix& Vm, Matrix& spikes) { 
    Vm.setZero();
    spikes.setZero();
    Row inh(1, p.time.steps_all()); inh.setZero();
    Row Is(1, p.time.steps_all());  Is.setZero();

    Column dKCdt;
    for (unsigned t = p.time.start_step()+1; t < p.time.steps_all(); t++) {
        double dIsdt = -Is(t-1) + (rv.kc.wKCAPL*spikes.col(t-1))(0,0)*1e4;
        double dinhdt = -inh(t-1) + Is(t-1);

        dKCdt = 
            -Vm.col(t-1)
            +rv.kc.wPNKC*pn_t.col(t)
            -rv.kc.wAPLKC*inh(t-1);
        Vm.col(t) = Vm.col(t-1) + dKCdt*p.time.dt/p.kc.taum;
        inh(t)    = inh(t-1)    + dinhdt*p.time.dt/p.kc.apl_taum;
        Is(t)     = Is(t-1)     + dIsdt*p.time.dt/p.kc.tau_apl2kc;

        auto const thr_comp = Vm.col(t).array() > rv.kc.thr.array();
        spikes.col(t) = thr_comp.select(1.0, spikes.col(t)); // either go to 1 or _stay_ at 0.
        Vm.col(t) = thr_comp.select(0.0, Vm.col(t)); // very abrupt repolarization!
    }
}

void run_ORN_LN_sims(ModelParams const& p, RunVars& rv) {
    rv.log("running ORN and LN sims");
#pragma omp parallel
    {
        Matrix orn_t(get_ngloms(p), p.time.steps_all());
        Row inhA(1, p.time.steps_all());
        Row inhB(1, p.time.steps_all());
#pragma omp for
        for (unsigned i = 0; i < get_nodors(p); i++) {
            sim_ORN_layer(p, rv, i, orn_t);
            sim_LN_layer(p, orn_t, inhA, inhB);
#pragma omp critical
            {
                rv.orn.sims[i] = orn_t;
                rv.ln.inhA.sims[i] = inhA;
                rv.ln.inhB.sims[i] = inhB;
            }

            /*
            sim_ORN_layer(p, rv, i, rv.orn.sims[i]);
            sim_LN_layer(
                    p, rv.orn.sims[i],
                    rv.ln.inhA.sims[i], rv.ln.inhB.sims[i]);
                    */
        }
    }
}
void run_PN_sims(ModelParams const& p, RunVars& rv) {
    rv.log("running PN sims");
#pragma omp parallel for
    for (unsigned i = 0; i < get_nodors(p); i++) {
        sim_PN_layer(
                p, rv,
                rv.orn.sims[i], rv.ln.inhA.sims[i], rv.ln.inhB.sims[i],
                rv.pn.sims[i]);
    }
}
void run_KC_sims(ModelParams const& p, RunVars& rv, bool regen) {
    if (regen) {
        rv.log("generating new KC replicate");
        build_wPNKC(p, rv);
        fit_sparseness(p, rv);
    }

    rv.log("running KC sims");
#pragma omp parallel
    {
        Matrix Vm(p.kc.N, p.time.steps_all());
        Matrix spikes(p.kc.N, p.time.steps_all());
        Matrix respcol;
        Matrix respcol_bin;
#pragma omp for
        for (unsigned i = 0; i < get_nodors(p); i++) {
            sim_KC_layer(
                    p, rv,
                    rv.pn.sims[i],
                    Vm, spikes);
            respcol = spikes.rowwise().sum();
            respcol_bin = (respcol.array() > 0.0).select(1.0, respcol);

#pragma omp critical
            rv.kc.responses.col(i) = respcol_bin;
            rv.kc.spike_counts.col(i) = respcol;
        }
    }
}

void remove_before(unsigned step, Matrix& timecourse) {
    Matrix intermediate = timecourse.block(
            0,                 step,
            timecourse.rows(), timecourse.cols()-step);
    timecourse = intermediate;
}
void remove_all_pretime(ModelParams const& p, RunVars& r) {
    auto cut = [&p](Matrix& m) {
        remove_before(p.time.start_step(), m);
    };
#pragma omp parallel
    {
        // ORN
#pragma omp for
        for (unsigned i = 0; i < r.orn.sims.size(); i++) {
            cut(r.orn.sims[i]);
        }
        // LN
#pragma omp for
        for (unsigned i = 0; i < r.ln.inhA.sims.size(); i++) {
            cut(r.ln.inhA.sims[i]);
        }
#pragma omp for
        for (unsigned i = 0; i < r.ln.inhB.sims.size(); i++) {
            cut(r.ln.inhB.sims[i]);
        }
        // PN
#pragma omp for
        for (unsigned i = 0; i < r.pn.sims.size(); i++) {
            cut(r.pn.sims[i]);
        }
    }
}
