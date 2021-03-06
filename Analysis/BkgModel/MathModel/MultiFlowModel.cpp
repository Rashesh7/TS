/* Copyright (C) 2010 Ion Torrent Systems, Inc. All Rights Reserved */

#include "MultiFlowModel.h"

using namespace std;

// wrapping ways to solve multiple flows simultaneously for the predicted trace



void FillBufferParamsBlockFlows ( buffer_params_block_flows *my_buff, bead_params *p, reg_params *reg_p, int *flow_ndx_map, int *buff_flow )
{
  int NucID;
  for ( int fnum=0;fnum<NUMFB;fnum++ )
  {
    NucID  =flow_ndx_map[fnum];
    // calculate some constants used for this flow
    my_buff->etbR[fnum] = AdjustEmptyToBeadRatioForFlow ( p->R,reg_p,NucID,buff_flow[fnum] );
    my_buff->tauB[fnum] = ComputeTauBfromEmptyUsingRegionLinearModel ( reg_p,my_buff->etbR[fnum] );
  }
}

void FillIncorporationParamsBlockFlows ( incorporation_params_block_flows *my_inc, bead_params *p, reg_params *reg_p,int *flow_ndx_map, int *buff_flow )
{
  int NucID;

  for ( int fnum=0; fnum<NUMFB; fnum++ )
    reg_p->copy_multiplier[fnum] = CalculateCopyDrift ( *reg_p, buff_flow[fnum] );
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {

    NucID=flow_ndx_map[fnum];
    my_inc->NucID[fnum] = NucID;
    my_inc->SP[fnum] = ( float ) ( COPYMULTIPLIER * p->Copies ) *reg_p->copy_multiplier[fnum];

    my_inc->sens[fnum] = reg_p->sens*SENSMULTIPLIER;
    my_inc->molecules_to_micromolar_conversion[fnum] = reg_p->molecules_to_micromolar_conversion;

    my_inc->d[fnum] = ( reg_p->d[NucID] ) *p->dmult;
    my_inc->kr[fnum] = reg_p->krate[NucID]*p->kmult[fnum];
    my_inc->kmax[fnum] = reg_p->kmax[NucID];
    my_inc->C[fnum] = reg_p->nuc_shape.C[NucID];
  }
}

void ApplyDarkMatter ( float *fval,reg_params *reg_p, vector<float>& dark_matter_compensator, int *flow_ndx_map, int npts )
{
  // dark matter vectorized in different loop
  float darkness = reg_p->darkness[0];
  float *dark_matter_for_flow;
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    dark_matter_for_flow = &dark_matter_compensator[flow_ndx_map[fnum]*npts];

    AddScaledVector ( fval+fnum*npts,dark_matter_for_flow,darkness,npts );
  }
}

void ApplyPCADarkMatter ( float *fval,bead_params *p, vector<float>& dark_matter_compensator, int npts )
{
  // dark matter vectorized in different loop
  float dm[npts];
  memset(dm,0,sizeof(dm));
  for (int icomp=0;icomp < NUM_DM_PCA;icomp++)
     AddScaledVector( dm,&dark_matter_compensator[icomp*npts],p->pca_vals[icomp],npts);

  for ( int fnum=0; fnum<NUMFB; fnum++ )
      AddScaledVector ( fval+fnum*npts,dm,1.0f,npts );
}


// 2nd-order background function with non-uniform bead well
void MultiFlowComputeTraceGivenIncorporationAndBackground ( float *fval,struct bead_params *p,struct reg_params *reg_p, float *ival, float *sbg,
    RegionTracker &my_regions, buffer_params_block_flows &cur_buffer_block,
    TimeCompression &time_c, flow_buffer_info &my_flow, bool use_vectorization, int bead_flow_t )
{
  float *vb_out[NUMFB];
  float *bkg_for_flow[NUMFB];
  float *new_hydrogen_for_flow[NUMFB];

  //@TODO: the natural place for vectorization is here at the flow level
  // flows are logically independent: apply "compute trace" to all flows
  // this makes for an obvious fit to solving 4 at once using the processor vectorization routines

  // parallel fill one bead parameter for block of flows
  FillBufferParamsBlockFlows ( &cur_buffer_block,p,reg_p,my_flow.flow_ndx_map,my_flow.buff_flow );

  // parallel compute across flows
  for ( int fnum=0;fnum<NUMFB;fnum++ )
  {
    vb_out[fnum] = fval + fnum*time_c.npts();        // get ptr to start of the function evaluation for the current flow
    bkg_for_flow[fnum] = &sbg[fnum*time_c.npts() ];            // get ptr to pre-shifted background
    new_hydrogen_for_flow[fnum] = &ival[fnum*time_c.npts() ];
  }

  // do the actual computation
#ifdef __INTEL_COMPILER
  {
    for ( int fnum=0; fnum<NUMFB; fnum++ )
      PurpleSolveTotalTrace ( vb_out[fnum],bkg_for_flow[fnum], new_hydrogen_for_flow[fnum], time_c.npts(),
                              &time_c.deltaFrame[0], cur_buffer_block.tauB[fnum], cur_buffer_block.etbR[fnum] );
  }
#else // assumed to be GCC
  if ( use_vectorization )
  {
    PurpleSolveTotalTrace_Vec ( NUMFB, vb_out, bkg_for_flow, new_hydrogen_for_flow, time_c.npts(),
                                &time_c.deltaFrame[0], cur_buffer_block.tauB, cur_buffer_block.etbR, p->gain );
  }
  else
  {
    for ( int fnum=0; fnum<NUMFB; fnum++ )
      PurpleSolveTotalTrace ( vb_out[fnum],bkg_for_flow[fnum], new_hydrogen_for_flow[fnum],time_c.npts(),
                              &time_c.deltaFrame[0], cur_buffer_block.tauB[fnum], cur_buffer_block.etbR[fnum] );
  }
#endif

  // adjust for well sensitivity, unexplained systematic effects
  // gain naturally parallel across flows
  MultiplyVectorByScalar ( fval,p->gain,bead_flow_t );

  // Dark Matter is extra background term of unexplained origin
  // Possibly should be applied directly to the observed signal rather than synthesized here inside a loop.
  if (my_regions.missing_mass.mytype == PerNucAverage)
     ApplyDarkMatter ( fval,reg_p,my_regions.missing_mass.dark_matter_compensator,my_flow.flow_ndx_map,time_c.npts() );
  else
     ApplyPCADarkMatter ( fval,p,my_regions.missing_mass.dark_matter_compensator,time_c.npts() );
}


void MultiFlowComputeCumulativeIncorporationSignal ( struct bead_params *p,struct reg_params *reg_p, float *ivalPtr,
    NucStep &cache_step, incorporation_params_block_flows &cur_bead_block,
    TimeCompression &time_c, flow_buffer_info &my_flow,  PoissonCDFApproxMemo *math_poiss )
{
  // only side effect should be new values in ivalPtr

  // this is region wide
  //This will short-circuit if has been computed
  cache_step.CalculateNucRiseCoarseStep ( reg_p,time_c,my_flow );
  // pretend I'm making a parallel process
  FillIncorporationParamsBlockFlows ( &cur_bead_block, p,reg_p,my_flow.flow_ndx_map,my_flow.buff_flow );
  // "In parallel, across flows"
  float* nuc_rise_ptr[NUMFB];
  float* incorporation_rise[NUMFB];
  int my_start[NUMFB];
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    nuc_rise_ptr[fnum]      = cache_step.NucCoarseStep ( fnum );
    incorporation_rise[fnum]= &ivalPtr[fnum*time_c.npts() ];
    my_start[fnum] = cache_step.i_start_coarse_step[fnum];
  }
  // this is >almost< a parallel operation by flows now
  bool use_my_parallel=true;
  if ( use_my_parallel )
  {
    //@TODO handle cases of fewer than 4 flows remaining
    for ( int fnum=0; fnum<NUMFB; fnum+=4 )
    {

      ParallelSimpleComputeCumulativeIncorporationHydrogens ( &incorporation_rise[fnum], time_c.npts(), &time_c.deltaFrameSeconds[0],&nuc_rise_ptr[fnum],
          ISIG_SUB_STEPS_MULTI_FLOW,  &my_start[fnum], &p->Ampl[fnum],
          &cur_bead_block.SP[fnum],&cur_bead_block.kr[fnum], &cur_bead_block.kmax[fnum], &cur_bead_block.d[fnum], &cur_bead_block.molecules_to_micromolar_conversion[fnum], math_poiss );
      for ( int q=0; q<4; q++ )
        MultiplyVectorByScalar ( incorporation_rise[fnum+q], cur_bead_block.sens[fnum+q],time_c.npts() );  // transform hydrogens to signal
    }
  }
  else
  {
    for ( int fnum=0;fnum<NUMFB;fnum++ )
    {
      ComputeCumulativeIncorporationHydrogens ( incorporation_rise[fnum], time_c.npts(),
          &time_c.deltaFrameSeconds[0], nuc_rise_ptr[fnum], ISIG_SUB_STEPS_MULTI_FLOW, my_start[fnum],
          cur_bead_block.C[fnum], p->Ampl[fnum], cur_bead_block.SP[fnum], cur_bead_block.kr[fnum], cur_bead_block.kmax[fnum], cur_bead_block.d[fnum],cur_bead_block.molecules_to_micromolar_conversion[fnum], math_poiss );

      MultiplyVectorByScalar ( incorporation_rise[fnum], cur_bead_block.sens[fnum],time_c.npts() );  // transform hydrogens to signal
    }
  }
}



void MultiCorrectBeadBkg ( float *block_signal_corrected, bead_params *p,
                           BeadScratchSpace &my_scratch, flow_buffer_info &my_flow, TimeCompression &time_c, RegionTracker &my_regions, float *sbg, bool use_vectorization )
{
  float vb[my_scratch.bead_flow_t];
  float* vb_out[my_flow.numfb];
  float* sbgPtr[my_flow.numfb];
  float block_bkg_plus_xtalk[my_scratch.bead_flow_t]; // set up instead of shifted background
  memset ( vb,0,sizeof ( float[my_scratch.bead_flow_t] ) );

  // add cross-talk for this bead to the empty-trace
  CopyVector ( block_bkg_plus_xtalk,sbg,my_scratch.bead_flow_t );
  AccumulateVector ( block_bkg_plus_xtalk,my_scratch.cur_xtflux_block,my_scratch.bead_flow_t );

  // compute the zeromer
  // setup pointers into the arrays
  for ( int fnum=0; fnum<my_flow.numfb; fnum++ )
  {
    // remove zeromer background - just like oneFlowFit.
    // should include xtalk (0) so I can reuse this routine
    sbgPtr[fnum] = &block_bkg_plus_xtalk[fnum*time_c.npts() ];
    vb_out[fnum] = &vb[fnum*time_c.npts() ];
  }

  // do the actual calculation in parallel or not
#ifdef __INTEL_COMPILER
  {
    for ( int fnum=0; fnum<my_flow.numfb; fnum++ )
      BlueSolveBackgroundTrace ( vb_out[fnum],sbgPtr[fnum],time_c.npts(),&time_c.deltaFrame[0],
                                 my_scratch.cur_buffer_block.tauB[fnum],my_scratch.cur_buffer_block.etbR[fnum] );
  }
#else
  if ( use_vectorization )
  {
    BlueSolveBackgroundTrace_Vec ( my_flow.numfb, vb_out, sbgPtr, time_c.npts(), &time_c.deltaFrame[0],
                                   my_scratch.cur_buffer_block.tauB, my_scratch.cur_buffer_block.etbR );
  }
  else
  {
    for ( int fnum=0; fnum<my_flow.numfb; fnum++ )
      BlueSolveBackgroundTrace ( vb_out[fnum],sbgPtr[fnum],time_c.npts(),&time_c.deltaFrame[0],
                                 my_scratch.cur_buffer_block.tauB[fnum],my_scratch.cur_buffer_block.etbR[fnum] );
  }
#endif

  MultiplyVectorByScalar ( vb,p->gain,my_scratch.bead_flow_t );

  if (my_regions.missing_mass.mytype == PerNucAverage)
     ApplyDarkMatter ( vb,&my_regions.rp, my_regions.missing_mass.dark_matter_compensator,my_flow.flow_ndx_map,time_c.npts() );
  else
     ApplyPCADarkMatter ( vb,p,my_regions.missing_mass.dark_matter_compensator,time_c.npts() );

  // zeromer computed, now remove from observed
  DiminishVector ( block_signal_corrected,vb,my_scratch.bead_flow_t ); // remove calculated background to produce corrected signal

}


void IonsFromBulk ( float **model_trace, float **incorporation_rise,
                    TimeCompression &time_c, RegionTracker &my_regions, flow_buffer_info my_flow,
                    bool use_vectorization,
                    float *vec_tau_bulk )
{
  // finally solve the way hydrogen ions diffuse out of the bulk
#ifdef __INTEL_COMPILER
  {
    for ( int fnum=0; fnum<NUMFB; fnum++ )
    {
      // Now solve the bulk
      RedSolveHydrogenFlowInWell ( model_trace[fnum],incorporation_rise[fnum],time_c.npts(),
                                   my_regions.cache_step.i_start_coarse_step[my_flow.flow_ndx_map[fnum]],&time_c.deltaFrame[0],vec_tau_bulk[fnum] ); // we retain hydrogen ions variably in the bulk depending on direction
    }
  }
#else
  if ( use_vectorization )
  {
    // Now solve the bulk
    RedSolveHydrogenFlowInWell_Vec ( NUMFB,model_trace,incorporation_rise,time_c.npts(),&time_c.deltaFrame[0],vec_tau_bulk );

  }
  else
  {
    for ( int fnum=0; fnum<NUMFB; fnum++ )
    {
      // Now solve the bulk
      RedSolveHydrogenFlowInWell ( model_trace[fnum],incorporation_rise[fnum],time_c.npts(),
                                   my_regions.cache_step.i_start_coarse_step[fnum],&time_c.deltaFrame[0],vec_tau_bulk[fnum] ); // we retain hydrogen ions variably in the bulk depending on direction

    }
  }
#endif
}


// note: input is incorporation_rise, returns lost_hydrogens in the same buffer, recycling the memory
void CumulativeLostHydrogens ( float **incorporation_rise_to_lost_hydrogens, float **scratch_trace, 
                               TimeCompression &time_c, RegionTracker &my_regions,
                               bool use_vectorization,
                               float *vec_tau_top )
{
  // Put the model trace from buffering the incorporation_rise into scratch_trace
#ifdef __INTEL_COMPILER
  {
    for ( int fnum=0; fnum<NUMFB; fnum++ )
    {
      RedSolveHydrogenFlowInWell ( scratch_trace[fnum],incorporation_rise_to_lost_hydrogens[fnum],time_c.npts(),my_regions.cache_step.i_start_coarse_step[fnum],&time_c.deltaFrame[0],vec_tau_top[fnum] ); // we lose hydrogen ions fast!

    }
  }
#else
  if ( use_vectorization )
  {
    RedSolveHydrogenFlowInWell_Vec ( NUMFB,scratch_trace,incorporation_rise_to_lost_hydrogens,time_c.npts(),&time_c.deltaFrame[0],vec_tau_top ); // we lose hydrogen ions fast!

  }
  else
  {
    for ( int fnum=0; fnum<NUMFB; fnum++ )
    {
      RedSolveHydrogenFlowInWell ( scratch_trace[fnum],incorporation_rise_to_lost_hydrogens[fnum],time_c.npts(),my_regions.cache_step.i_start_coarse_step[fnum],&time_c.deltaFrame[0],vec_tau_top[fnum] ); // we lose hydrogen ions fast!

    }
  }
#endif

  // return lost_hydrogens in the incorporation_rise variables by subtracting the trace from the cumulative
  for ( int fnum=0; fnum<NUMFB; fnum++ )
    DiminishVector ( incorporation_rise_to_lost_hydrogens[fnum],scratch_trace[fnum],time_c.npts() );  // cumulative lost hydrogen ions instead of retained hydrogen ions

}

void IncorporationRiseFromNeighborParameters ( float **incorporation_rise, float **nuc_rise_ptr,
    bead_params *p,
    TimeCompression &time_c, RegionTracker &my_regions,
    BeadScratchSpace &my_scratch, PoissonCDFApproxMemo *math_poiss )
{
  // fill in each flow incorporation
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    // compute newly generated ions for the amplitude of each flow
    ComputeCumulativeIncorporationHydrogens ( incorporation_rise[fnum],
        time_c.npts(), &time_c.deltaFrameSeconds[0], nuc_rise_ptr[fnum], ISIG_SUB_STEPS_MULTI_FLOW, my_regions.cache_step.i_start_coarse_step[fnum],
        my_scratch.cur_bead_block.C[fnum], p->Ampl[fnum], my_scratch.cur_bead_block.SP[fnum], my_scratch.cur_bead_block.kr[fnum], my_scratch.cur_bead_block.kmax[fnum], my_scratch.cur_bead_block.d[fnum],my_scratch.cur_bead_block.molecules_to_micromolar_conversion[fnum], math_poiss );
    MultiplyVectorByScalar ( incorporation_rise[fnum], my_scratch.cur_bead_block.sens[fnum],time_c.npts() );  // transform hydrogens to signal       // variables used for solving background signal shape
  }
}

// rescale final trace by differences in buffering between wells
// wells with more buffering shield from the total hydrogens floating around
void RescaleTraceByBuffering ( float **my_trace, float *vec_tau_source, float *vec_tau_dest, int npts )
{
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    MultiplyVectorByScalar ( my_trace[fnum],vec_tau_source[fnum],npts );
    MultiplyVectorByScalar ( my_trace[fnum],1.0f/vec_tau_dest[fnum],npts );
  }
}


// Multiflow: Solve incorporation
// Solve lost hydrogens to bulk
// solve bulk resistance to lost hydrogens
// this function too large, should be componentized
void AccumulateSingleNeighborXtalkTrace ( float *my_xtflux, bead_params *p, reg_params *reg_p,
    BeadScratchSpace &my_scratch, TimeCompression &time_c, RegionTracker &my_regions, flow_buffer_info my_flow,
    PoissonCDFApproxMemo *math_poiss, bool use_vectorization,
    float tau_top, float tau_bulk, float multiplier )
{

  // Compute the hydrogen signal of xtalk in the bulk fluid above the affected well
  // 1) Xtalk happens fast -> we compute the cumulative lost hydrogen ions at the top of the bead instead of at the bottom (tau_top)
  // 1a) xtalk happens as the well loses hydrogen ions so the cumulative lost = cumulative total generated - number in well currently
  // 2) hydrogen ions are "retained" by the bulk for a finite amount of time as they are swept along
  // 2a) this 'retainer' may be asymmetric (tau_bulk) models the decay rate
  // 3) multiplier: hydrogen ions spread out, so are of different proportion to the original signal

  // this is pre-calculated outside for the current region parameters
  //my_regions.cache_step.CalculateNucRiseCoarseStep(reg_p,time_c);


  // over-ride buffering parameters for bead
  // use the same incorporation parameters, though
  FillIncorporationParamsBlockFlows ( &my_scratch.cur_bead_block, p,reg_p,my_flow.flow_ndx_map,my_flow.buff_flow );
  //params_IncrementHits(p);

  float block_model_trace[my_scratch.bead_flow_t], block_incorporation_rise[my_scratch.bead_flow_t];

  // "In parallel, across flows"
  float* nuc_rise_ptr[NUMFB];
  float* model_trace[NUMFB];
  float* scratch_trace[NUMFB];
  float* incorporation_rise[NUMFB];
  float* lost_hydrogens[NUMFB];
  // should this be using cur_buffer_block as usual?
  float vec_tau_top[NUMFB];
  float vec_tau_bulk[NUMFB];

  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    vec_tau_top[fnum] = tau_top;
    vec_tau_bulk[fnum] = tau_bulk;
  }

  // setup parallel pointers into the structure
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    nuc_rise_ptr[fnum] = my_regions.cache_step.NucCoarseStep ( fnum );
    scratch_trace[fnum]=model_trace[fnum] = &block_model_trace[fnum*time_c.npts() ];
    lost_hydrogens[fnum]=incorporation_rise[fnum] = &block_incorporation_rise[fnum*time_c.npts() ];   // set up each flow information
  }

  IncorporationRiseFromNeighborParameters ( incorporation_rise, nuc_rise_ptr, p, time_c, my_regions, my_scratch, math_poiss );
  // temporarily use the model_trace memory structure as scratch space
  // turn incorporation_rise into lost hydrogens
  CumulativeLostHydrogens ( incorporation_rise, scratch_trace,time_c, my_regions, use_vectorization, vec_tau_top );
  // lost_hydrogens = incorporation_rise
  // now fill in the model_trace structure for real, overwriting any temporary use of that space
  IonsFromBulk ( model_trace,lost_hydrogens, time_c, my_regions, my_flow, use_vectorization, vec_tau_bulk );

  // universal
  MultiplyVectorByScalar ( block_model_trace,multiplier,my_scratch.bead_flow_t ); // scale down the quantity of ions

  // add to the bulk cross-talk we're creating
  AccumulateVector ( my_xtflux, block_model_trace,my_scratch.bead_flow_t );
}

void IncorporationRiseFromNeighborSignal ( float **incorporation_rise, float **neighbor_local,
    TimeCompression &time_c, RegionTracker &my_regions,
    BeadScratchSpace &my_scratch
                                         )
{
  // fill in each flow incorporation
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    // grab my excess hydrogen ions from the observed signal
    IntegrateRedFromObservedTotalTrace ( incorporation_rise[fnum], neighbor_local[fnum], &my_scratch.shifted_bkg[fnum*time_c.npts() ], time_c.npts(),&time_c.deltaFrame[0], my_scratch.cur_buffer_block.tauB[fnum], my_scratch.cur_buffer_block.etbR[fnum] );
    // variables used for solving background signal shape
  }

  // construct the incorporation in the neighbor well just for research purposes
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    // bad! put trace back in neighbor signal
    RedSolveHydrogenFlowInWell ( neighbor_local[fnum],incorporation_rise[fnum],time_c.npts(),my_regions.cache_step.i_start_coarse_step[fnum],&time_c.deltaFrame[0],my_scratch.cur_buffer_block.tauB[fnum] ); // we lose hydrogen ions fast!
  }
}



void AccumulateSingleNeighborExcessHydrogen ( float *my_xtflux, float *neighbor_signal, bead_params *p, reg_params *reg_p,
    BeadScratchSpace &my_scratch, TimeCompression &time_c,
    RegionTracker &my_regions, flow_buffer_info my_flow,
    bool use_vectorization,
    float tau_top, float tau_bulk, float multiplier )
{
  // over-ride buffering parameters for bead

  FillBufferParamsBlockFlows ( &my_scratch.cur_buffer_block,p,reg_p,my_flow.flow_ndx_map,my_flow.buff_flow );

  float block_model_trace[my_scratch.bead_flow_t], block_incorporation_rise[my_scratch.bead_flow_t];

  // "In parallel, across flows"

  float* model_trace[NUMFB];
  float* scratch_trace[NUMFB];
  float* incorporation_rise[NUMFB];
  float* lost_hydrogens[NUMFB];
  float* neighbor_local[NUMFB];
  // should this be using cur_buffer_block as usual?
  float vec_tau_top[NUMFB];
  float vec_tau_bulk[NUMFB];

  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    vec_tau_top[fnum] = tau_top;
    vec_tau_bulk[fnum] = tau_bulk;
  }

  // setup parallel pointers into the structure
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {

    scratch_trace[fnum]=model_trace[fnum] = &block_model_trace[fnum*time_c.npts() ];
    lost_hydrogens[fnum]=incorporation_rise[fnum] = &block_incorporation_rise[fnum*time_c.npts() ];   // set up each flow information
    neighbor_local[fnum] = &neighbor_signal[fnum*time_c.npts() ];
  }
  // make incorporation_rise
  IncorporationRiseFromNeighborSignal ( incorporation_rise,neighbor_local, time_c,my_regions, my_scratch );
  // use scratch_trace to hold temporary trace - same memory as model_trace because we don't need it yet
  // turn incorporation_rise into lost_hydrogens
  CumulativeLostHydrogens ( incorporation_rise, scratch_trace,time_c, my_regions, use_vectorization, vec_tau_top );
  // lost_hydrogens = incorporation_rise 
  // now get model_trace for real in cross-talk and overwrite any temporary uses of that space
  IonsFromBulk ( model_trace,lost_hydrogens, time_c, my_regions, my_flow, use_vectorization, vec_tau_bulk );
  

  // universal
  MultiplyVectorByScalar ( block_model_trace,multiplier,my_scratch.bead_flow_t ); // scale down the quantity of ions

  // add to the bulk cross-talk we're creating
  AccumulateVector ( my_xtflux, block_model_trace,my_scratch.bead_flow_t );
}

// Multiflow: Solve incorporation
// Solve lost hydrogens to bulk
// simplify cross-talk
// a) well >loses< ions - excess hydrogen lost based on well parameters
// b) "empty" well >gains< ions - based on tauE
// c) "gained" ions will then predict bulk

void AccumulateSingleNeighborExcessHydrogenOneParameter ( float *my_xtflux, float *neighbor_signal,
    bead_params *p, reg_params *reg_p,
    BeadScratchSpace &my_scratch, TimeCompression &time_c,
    RegionTracker &my_regions, flow_buffer_info my_flow,
    bool use_vectorization,
    float multiplier, bool rescale_flag )
{

  FillBufferParamsBlockFlows ( &my_scratch.cur_buffer_block,p,reg_p,my_flow.flow_ndx_map,my_flow.buff_flow );

  float block_model_trace[my_scratch.bead_flow_t], block_incorporation_rise[my_scratch.bead_flow_t];

  // "In parallel, across flows"

  float* model_trace[NUMFB];
  float* scratch_trace[NUMFB];
  float* incorporation_rise[NUMFB];
  float* lost_hydrogens[NUMFB];
  float* neighbor_local[NUMFB];

  // should this be using cur_buffer_block as usual?
  float vec_tau_well[NUMFB];
  float vec_tau_empty[NUMFB];

  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {
    vec_tau_well[fnum] = my_scratch.cur_buffer_block.tauB[fnum];
    vec_tau_empty[fnum] = my_scratch.cur_buffer_block.etbR[fnum]*vec_tau_well[fnum];
  }

  // setup parallel pointers into the structure
  for ( int fnum=0; fnum<NUMFB; fnum++ )
  {

    scratch_trace[fnum] = model_trace[fnum] = &block_model_trace[fnum*time_c.npts() ];
    lost_hydrogens[fnum] = incorporation_rise[fnum] = &block_incorporation_rise[fnum*time_c.npts() ];   // set up each flow information
    neighbor_local[fnum] = &neighbor_signal[fnum*time_c.npts() ];
  }

  IncorporationRiseFromNeighborSignal ( incorporation_rise,neighbor_local, time_c,my_regions, my_scratch );
  // uses a scratch buffer here [recycles model_trace as we don't need it yet], 
  // turns incorporation_rise into lost_hydrogens
  CumulativeLostHydrogens ( incorporation_rise, scratch_trace, time_c, my_regions, use_vectorization, vec_tau_well );
  // lost_hydrogens=incorporation_rise returned as lost_hydrogens above
  // now we generate the real model_trace we're accumulating
  IonsFromBulk ( model_trace,lost_hydrogens, time_c, my_regions, my_flow, use_vectorization, vec_tau_empty );
  
  if (rescale_flag)
    RescaleTraceByBuffering(model_trace, vec_tau_well, vec_tau_empty,time_c.npts());

  // universal
  MultiplyVectorByScalar ( block_model_trace,multiplier,my_scratch.bead_flow_t ); // scale down the quantity of ions

  // add to the bulk cross-talk we're creating
  AccumulateVector ( my_xtflux, block_model_trace,my_scratch.bead_flow_t );
}

