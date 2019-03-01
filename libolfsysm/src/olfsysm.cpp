#include "olfsysm.hpp"

#include <math.h>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <iostream>
#include <functional>

/* The ID associated with each HC glom, in the order that they are listed as
 * columns in the HC data file.
 * len = N_HC_GLOMS+1 = 24. */
unsigned const HC_GLOMNUMS[] = {
    6, 16, 45, 11, 7, 19, 4,
    123456,  // UNUSED!! (8TH GLOM)
    38, 5, 44, 20, 28, 32, 21,
    14, 23, 39, 33, 22, 47, 15,
    27, 48};

/* A transform that zeros rows corresponding to non-HC gloms. */
Matrix const ZERO_NONHC_GLOMS = [](){
    Matrix ret(N_GLOMS_ALL, N_GLOMS_ALL);
    ret.setZero();
    for (unsigned i = 0; i < N_HC_GLOMS+1; i++) {
        if (i == 7) continue; // skip the 8th glom; it's bad!
        unsigned gn = HC_GLOMNUMS[i];
        ret(gn, gn) = 1.0;
    }
    return ret;
}();

/* A distribution describing the frequency with wich each HC glom should be
 * connected to when creating PN->KC connectivity matrices. */
std::discrete_distribution<int> HC_GLOM_CXN_DISTRIB {
    2.0, 24.0, 4.0, 30.0, 33.0, 8.0, 0.0,
    0.0, // no #8!
    29.0, 6.0, 2.0, 4.0, 21.0, 18.0, 4.0,
    12.0, 21.0, 10.0, 27.0, 4.0, 26.0, 7.0,
    26.0, 24.0
};
/* Convenience description of uniform connectivity probabilities, with #8
 * removed. */
std::discrete_distribution<int> HC_GLOM_CXN_UNIFORM_DISTRIB {
    1, 1, 1, 1, 1, 1, 1,
    0, // no #8!
    1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    1, 1
};

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

    p.orn.taum = 0.01;
    p.orn.hcdata_path = "hc_data.csv";

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

    p.kc.N           = 2000;
    p.kc.nclaws      = 6;
    p.kc.uniform_pns = false;
    p.kc.enable_apl  = true;
    p.kc.sp_target   = 0.1;
    p.kc.sp_acc      = 0.1;
    p.kc.taum        = 0.01;
    p.kc.apl_taum    = 0.05;
    p.kc.tau_apl2kc  = 0.01;

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
    rates(N_GLOMS, N_ODORS),
    spont(N_GLOMS, 1),
    delta(N_GLOMS, N_ODORS),
    sims(N_ODORS, Matrix(N_GLOMS, p.time.steps_all())) {
}
RunVars::LN::LN(ModelParams const& p) :
    inhA{std::vector<Vector>(N_HC_ODORS, Row(1, p.time.steps_all()))},
    inhB{std::vector<Vector>(N_HC_ODORS, Row(1, p.time.steps_all()))} {
}
RunVars::PN::PN(ModelParams const& p) :
    sims(N_ODORS, Matrix(N_GLOMS, p.time.steps_all())) {
}
RunVars::KC::KC(ModelParams const& p) :
    wPNKC(p.kc.N, N_GLOMS),
    wAPLKC(p.kc.N, 1),
    wKCAPL(1, p.kc.N),
    thr(p.kc.N, 1),
    responses(p.kc.N, N_ODORS) {
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

void load_hc_data(ModelParams const& p, RunVars& run) {
    run.orn.rates.setZero();
    run.orn.spont.setZero();
    run.orn.delta.setZero();

    std::ifstream fin(p.orn.hcdata_path);
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
        split_regular_csv(line, segs);
        for (unsigned glom = 0; glom < N_HC_GLOMS+1; glom++) {
            /* Ignore the 8th glom column (Kennedy does this). */
            if (glom == 7) continue;

            /* At this point we're actually storing deltas. */
            run.orn.rates(HC_GLOMNUMS[glom], odor) = std::stod(segs[glom+2]);
            run.orn.delta(HC_GLOMNUMS[glom], odor) = std::stod(segs[glom+2]);
        }
    }

    /* Load the spontaneous rates line. */
    std::getline(fin, line);
    split_regular_csv(line, segs);
    for (unsigned glom = 0; glom < N_HC_GLOMS+1; glom++) {
        if (glom == 7) continue;
        run.orn.spont(HC_GLOMNUMS[glom]) = std::stod(segs[glom+2]);
    }

    /* Convert deltas into absolute rates. */
    run.orn.rates.colwise() += run.orn.spont.col(0);
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

void build_wPNKC_weighted(ModelParams const& p, RunVars& r) {
    /* Draw PN connections from realistic connection distribution data (see
     * above). */
    r.kc.wPNKC.setZero();
    for (unsigned kc = 0; kc < p.kc.N; kc++) {
        for (unsigned claw = 0; claw < p.kc.nclaws; claw++) {
            r.kc.wPNKC(kc, HC_GLOMNUMS[HC_GLOM_CXN_DISTRIB(g_randgen)]) += 1.0;
        }
    }
}
void build_wPNKC_uniform(ModelParams const& p, RunVars& r) {
    r.kc.wPNKC.setZero();
    for (unsigned kc = 0; kc < p.kc.N; kc++) {
        for (unsigned claw = 0; claw < p.kc.nclaws; claw++) {
            r.kc.wPNKC(kc, HC_GLOMNUMS[
                    HC_GLOM_CXN_UNIFORM_DISTRIB(g_randgen)]) += 1.0;
        }
    }
}
void build_wPNKC(ModelParams const& p, RunVars& rv) {
    if (p.kc.uniform_pns) {
        build_wPNKC_uniform(p, rv);
    }
    else {
        build_wPNKC_weighted(p, rv);
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
    return rv.pn.sims[0].block(0,sp_t1,N_GLOMS,sp_t2-sp_t1).rowwise().mean();
}
Column choose_KC_thresh(
        ModelParams const& p, Matrix& KCpks, Column const& spont_in) {
    KCpks.resize(1, KCpks.size());                     // flatten
    std::sort(KCpks.data(), KCpks.data()+KCpks.size(),
            [](double a, double b){return a>b;});      // dec. order
    double thr_const = KCpks(std::min(
                int(p.kc.sp_target*2.0*double(p.kc.N*N_ODORS)),
                int(p.kc.N*N_ODORS)-1));
    return thr_const + spont_in.array()*2.0;

}
void fit_sparseness(ModelParams const& p, RunVars& rv) {
    /* Set starting values for the things we'll tune. */
    rv.kc.wAPLKC.setZero();
    rv.kc.wKCAPL.setConstant(1.0/float(p.kc.N));
    rv.kc.thr.setConstant(1e5); // higher than will ever be reached (for now)

    /* Calculate spontaneous input to KCs. */
    Column spont_in = rv.kc.wPNKC * sample_PN_spont(p, rv);

    /* Used for measuring KC voltage; defined here to make it shared across all
     * threads.*/
    Matrix KCpks(p.kc.N, N_ODORS); KCpks.setZero();

    /* Used to store odor response data during APL tuning. */
    Matrix KCmean_st(p.kc.N, 1+((N_ODORS-1)/3));
    /* Used to store the current sparsity.
     * Initially set to the below value because, given default model
     * parameters, it causes tuning to complete in just one iteration. */
    double sp = 0.0789;
    /* Used to count number of times looped; the 'learning rate' is decreased
     * as 1/sqrt(count) with each iteration. */
    double count = 1.0;

    /* Break up into threads. */
#pragma omp parallel
    { 
        /* Output matrices for the KC simulation. */
        Matrix Vm(p.kc.N, p.time.steps_all());
        Matrix spikes(p.kc.N, p.time.steps_all());

        /* Measure voltages achieved by the KCs, and choose a threshold based
         * on that. */
#pragma omp for
        for (unsigned i = 0; i < N_ODORS; i++) {
            sim_KC_layer(p, rv, rv.pn.sims[i], Vm, spikes);
            KCpks.col(i) = Vm.rowwise().maxCoeff() - spont_in*2.0;
        }

#pragma omp single
        {
            /* Finish picking thresholds. */
            rv.kc.thr = choose_KC_thresh(p, KCpks, spont_in);
        }

        /* Enter this region only if APL use is enabled; if disabled, just exit
         * (at this point APL->KC weights are set to 0). */
        if (p.kc.enable_apl) {

#pragma omp single
        {
            /* Starting values for to-be-tuned APL<->KC weights. */
            rv.kc.wAPLKC.setConstant(
                    2*ceil(-log(p.kc.sp_target)));
            rv.kc.wKCAPL.setConstant(
                    2*ceil(-log(p.kc.sp_target))/double(p.kc.N));
        }

        /* Continue tuning until we reach the desired sparsity. */
        do {
#pragma omp single
            {
                /* Modify the APL<->KC weights in order to move in the
                 * direction of the target sparsity. */
                double lr = 10.0/sqrt(count);
                double delta = (sp-p.kc.sp_target)*lr/p.kc.sp_target;
                rv.kc.wAPLKC.array() += delta;
                rv.kc.wKCAPL.array() += delta/double(p.kc.N);

                count += 1.0;
            }

            /* Run through a bunch of odors to test sparsity. */
#pragma omp for
            for (unsigned i = 0; i < N_ODORS; i+=3) {
                sim_KC_layer(p, rv, rv.pn.sims[i], Vm, spikes);
                KCmean_st.col(i/3) = spikes.rowwise().sum();
            }

#pragma omp single
            {
                KCmean_st = (KCmean_st.array() > 0.0).select(1.0, KCmean_st);
                sp = KCmean_st.mean();
            }
        } while (abs(sp-p.kc.sp_target)>(p.kc.sp_acc*p.kc.sp_target));
    }}
}

void sim_ORN_layer(
        ModelParams const& p, RunVars const& rv,
        int odorid,
        Matrix& orn_t) {
    /* Initialize with spontaneous activity. */
    orn_t = rv.orn.spont*p.time.row_all();

    /* "Odor input to ORNs" (Kennedy comment)
     * Smoothed timeseries of spont...odor rate...spont */
    Matrix odor = orn_t + rv.orn.delta.col(odorid)*p.time.stim.row_all();
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
    for (unsigned t = 1; t < p.time.steps_all(); t++) {
        dinhAdt = -inhA(t-1) + response(t-1);
        dinhBdt = -inhB(t-1) + response(t-1);
        dLNdt =
            -potential(t-1)
            +pow(orn_t.col(t-1).mean(), 3.0)*51.0/23.0/2.0*inh_LN;
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

    Column spont  = rv.orn.spont*p.pn.inhsc/(rv.orn.spont.sum()+p.pn.inhadd);
    pn_t          = rv.orn.spont*p.time.row_all();
    double inh_PN = 0.0;

    Column orn_delta;
    Column dPNdt;
    for (unsigned t = 1; t < p.time.steps_all(); t++) {
        orn_delta = orn_t.col(t-1)-rv.orn.spont;
        dPNdt = -pn_t.col(t-1) + spont;
        dPNdt += 
            200.0*((orn_delta.array()+p.pn.offset)*p.pn.tanhsc/200.0*inh_PN).matrix().unaryExpr<double(*)(double)>(&tanh);
        add_randomly([&noise](){return noise(g_randgen);}, dPNdt);

        inh_PN = p.pn.inhsc/(p.pn.inhadd+0.25*inhA(t)+0.75*inhB(t));
        pn_t.col(t) = pn_t.col(t-1) + dPNdt*p.time.dt/p.pn.taum;
        pn_t.col(t) = (0.0 < pn_t.col(t).array()).select(pn_t.col(t), 0.0);
    }

    /* Zero non-HC gloms (they are just noise, not from odor...) */
    pn_t = ZERO_NONHC_GLOMS * pn_t;
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
#pragma omp parallel
    {
        Matrix orn_t(N_GLOMS, p.time.steps_all());
        Row inhA(1, p.time.steps_all());
        Row inhB(1, p.time.steps_all());
#pragma omp for
        for (unsigned i = 0; i < N_ODORS; i++) {
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
#pragma omp parallel for
    for (unsigned i = 0; i < N_ODORS; i++) {
        sim_PN_layer(
                p, rv,
                rv.orn.sims[i], rv.ln.inhA.sims[i], rv.ln.inhB.sims[i],
                rv.pn.sims[i]);
    }
}
void run_KC_sims(ModelParams const& p, RunVars& rv, bool regen) {
    if (regen) {
        build_wPNKC(p, rv);
        fit_sparseness(p, rv);
    }

#pragma omp parallel
    {
        Matrix Vm(p.kc.N, p.time.steps_all());
        Matrix spikes(p.kc.N, p.time.steps_all());
        Matrix respcol;
#pragma omp for
        for (unsigned i = 0; i < N_ODORS; i++) {
            sim_KC_layer(
                    p, rv,
                    rv.pn.sims[i],
                    Vm, spikes);
            respcol = spikes.rowwise().sum();
            respcol = (respcol.array() > 0.0).select(1.0, respcol);

#pragma omp critical
            rv.kc.responses.col(i) = respcol;
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
