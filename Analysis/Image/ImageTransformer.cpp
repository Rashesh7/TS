/* Copyright (C) 2012 Ion Torrent Systems, Inc. All Rights Reserved */
#include "ImageTransformer.h"
#include "LSRowImageProcessor.h"
#include "Utils.h"
#include "Image.h"
#include "IonErr.h"
#include "SynchDat.h"

//Initialize chipSubRegion in Image class
#define MAX_GAIN_CORRECT 16383

Region ImageCropping::chipSubRegion(0,0,0,0);
int ImageCropping::cropped_region_offset_x = 0;
int ImageCropping::cropped_region_offset_y = 0;

#define CHANXT_VEC_SIZE    8
#define CHANXT_VEC_SIZE_B 32
typedef float ChanVecf_t __attribute__ ((vector_size (CHANXT_VEC_SIZE_B)));
typedef union{
	ChanVecf_t V;
	float A[CHANXT_VEC_SIZE];
}ChanVecf_u;


// this is perhaps a prime candidate for something that is a json set of parameters to read in
// inversion vector + coordinates of the offsets
// we do only a single pass because the second-order correction is too small to notice

static float chan_xt_vect_316[DEFAULT_VECT_LEN]      = {0.0029,-0.0632,-0.0511,1.1114,0.0000,0.0000,0.0000};
static float *default_316_xt_vectors[] = {chan_xt_vect_316};

static float chan_xt_vect_318_even[DEFAULT_VECT_LEN] = {0.0132,-0.1511,-0.0131,1.1076,0.0404,0.0013,0.0018};
static float chan_xt_vect_318_odd[DEFAULT_VECT_LEN]  = {0.0356,-0.1787,-0.1732,1.3311,-0.0085,-0.0066,0.0001};
static float *default_318_xt_vectors[] = {chan_xt_vect_318_even, chan_xt_vect_318_even,chan_xt_vect_318_even, chan_xt_vect_318_even,
    chan_xt_vect_318_odd, chan_xt_vect_318_odd,chan_xt_vect_318_odd, chan_xt_vect_318_odd
                                          };
int ImageTransformer::chan_xt_column_offset[DEFAULT_VECT_LEN]   = {-12,-8,-4,0,4,8,12};

ChipXtVectArrayType ImageTransformer::default_chip_xt_vect_array[] = {
  {ChipId316, {default_316_xt_vectors, 1, DEFAULT_VECT_LEN, chan_xt_column_offset} },
  {ChipId318, {default_318_xt_vectors, 8, DEFAULT_VECT_LEN, chan_xt_column_offset} },
  {ChipIdUnknown, {NULL, 0,0,NULL} },
};

ChannelXTCorrectionDescriptor ImageTransformer::selected_chip_xt_vectors = {NULL, 0,0,NULL};

 int ImageTransformer::dump_XTvects_to_file=1; // we'll flip this after the first time we write to disk what the vectors are

// XTChannelCorrect:
// For the 316 and 318, corrects cross-talk due to incomplete analog settling within the 316 and 318, and also
// residual uncorrected incomplete setting at the output of the devices.
// works along each row of the image and corrects cross talk that occurs
// within a single acquisition channel (every fourth pixel is the same channel on the 316/318)
// This method has no effect on the 314.
// The following NOTE is out-of-date, something like this may come back sometime
// NOTE:  A side-effect of this method is that the data for all pinned pixels will be replaced with
// the average of the surrounding neighbor pixels.  This helps limit the spread of invalid data in the pinned
// pixels to neighboring wells
// void Image::XTChannelCorrect (Mask *mask)
void ImageTransformer::XTChannelCorrect(RawImage *raw,
		const char *experimentName) {

	float **vects = NULL;
	int nvects = 0;
	int *col_offset = NULL;
	int vector_len;
	int frame, row, col, vn;
	short *pfrm, *prow;
	int i, lc;
	uint32_t vndx;

	// If no correction has been configured for (by a call to CalibrateChannelXTCorrection), the try to find the default
	// correction using the chip id as a guide.
	if (selected_chip_xt_vectors.xt_vector_ptrs == NULL)
		for (int nchip = 0;
				default_chip_xt_vect_array[nchip].id != ChipIdUnknown; nchip++)
			if (default_chip_xt_vect_array[nchip].id
					== ChipIdDecoder::GetGlobalChipId()) {
				memcpy(&selected_chip_xt_vectors,
						&(default_chip_xt_vect_array[nchip].descr),
						sizeof(selected_chip_xt_vectors));
				break;
			}

	// if the chip type is unsupported, silently return and do nothing
	if (selected_chip_xt_vectors.xt_vector_ptrs == NULL)
		return;

	vects = selected_chip_xt_vectors.xt_vector_ptrs;
	nvects = selected_chip_xt_vectors.num_vectors;
	col_offset = selected_chip_xt_vectors.vector_indicies;
	vector_len = selected_chip_xt_vectors.vector_len;

	// fill in pinned pixels with average of surrounding valid wells
	//BackgroundCorrect(mask, MaskPinned, (MaskType)(MaskAll & ~MaskPinned & ~MaskExclude),0,5,false,false,true);
	if ((raw->cols % 8) != 0) {
		short tmp[raw->cols];
		float *vect;
		int ndx;
		float sum;

		for (frame = 0; frame < raw->frames; frame++) {
			pfrm = &(raw->image[frame * raw->frameStride]);
			for (row = 0; row < raw->rows; row++) {
				prow = pfrm + row * raw->cols;
				for (col = 0; col < raw->cols; col++) {
					vndx = ((col + ImageCropping::cropped_region_offset_x)
							% nvects);
					vect = vects[vndx];

					sum = 0.0;
					for (vn = 0; vn < vector_len; vn++) {
						ndx = col + col_offset[vn];
						if ((ndx >= 0) && (ndx < raw->cols))
							sum += prow[ndx] * vect[vn];
					}
					tmp[col] = (short) (sum);
				}
				// copy result back into the image
				memcpy(prow, tmp, sizeof(short[raw->cols]));
			}
		}

	} else {
//#define XT_TEST_CODE
#ifdef XT_TEST_CODE
		short *tstImg = (short *)malloc(raw->rows*raw->cols*2*raw->frames);
		memcpy(tstImg,raw->image,raw->rows*raw->cols*2*raw->frames);
		{
			short tmp[raw->cols];
			float *vect;
			int ndx;
			float sum;

			for ( frame = 0;frame < raw->frames;frame++ ) {
				pfrm = & ( tstImg[frame*raw->frameStride] );
				for ( row = 0;row < raw->rows;row++ ) {
					prow = pfrm + row*raw->cols;
					for ( col = 0;col < raw->cols;col++ ) {
						vndx = ( ( col+ImageCropping::cropped_region_offset_x ) % nvects );
						vect = vects[vndx];

						sum = 0.0;
						for ( vn = 0;vn < vector_len;vn++ ) {
							ndx = col + col_offset[vn];
							if ( ( ndx >= 0 ) && ( ndx < raw->cols ) )
							sum += prow[ndx]*vect[vn];
						}
						tmp[col] = ( short ) ( sum );
					}
					// copy result back into the image
					memcpy ( prow,tmp,sizeof ( short[raw->cols] ) );
				}
			}
		}
#endif
		{
			ChanVecf_u vectsV[8][7];
			ChanVecf_u Avect[4], Svect[4];
			uint32_t j;

			for (vn = 0; vn < nvects; vn++) {
				for (i = 0; i < 8; i++) {
					if (i < vector_len)
						vectsV[vn][6].A[i] = vects[vn][i];
					else
						vectsV[vn][6].A[i] = 0;
				}
			}
			for (vn = 0; vn < nvects; vn++) {
				for (j = 0; j < 6; j++) {
					for (i = 0; i < 7; i++) {
						vectsV[vn][j].A[i] = vectsV[vn][6].A[(i + 7 - (j + 1))
								% 7];
					}
					vectsV[vn][j].A[7] = 0;
				}
			}

#ifdef XT_TEST_CODE
			static int doneOnce=0;
			if(!doneOnce)
			{
				doneOnce=1;
				for(vn=0;vn<nvects;vn++)
				{
					printf("vn=%d\n",vn);
					for(j=0;j<7;j++)
					{
						printf(" %d(%d) %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",vn,j,
								vectsV[vn][j].A[0],vectsV[vn][j].A[1],vectsV[vn][j].A[2],
								vectsV[vn][j].A[3],vectsV[vn][j].A[4],vectsV[vn][j].A[5],
								vectsV[vn][j].A[6],vectsV[vn][j].A[7]);
					}
				}

			}
#endif
			for (frame = 0; frame < raw->frames; frame++) {
				pfrm = &(raw->image[frame * raw->frameStride]);
				for (row = 0; row < raw->rows; row++) {
					prow = pfrm + row * raw->cols;

					// prime the Avect values
					for (lc = 0; lc < 4; lc++) {
						Avect[lc].A[0] = 0; // -12
						Avect[lc].A[1] = 0; // -8
						Avect[lc].A[2] = 0; // -4
						Avect[lc].A[3] = prow[lc]; //  0
						Avect[lc].A[4] = prow[lc + 4]; //  4
						Avect[lc].A[5] = prow[lc + 8]; //  8
						Avect[lc].A[6] = 0; //  12
						Avect[lc].A[7] = 0;
					}
					for (col = 0, j = 6; col < raw->cols;
							col += 4, j = ((j + 1) % 7)) {

						// fill in the last values...
						if ((col + 16) <= raw->cols) {
							for (lc = 0; lc < 4; lc++)
								Avect[lc].A[j] = prow[col + 12 + lc];
						} else {
							for (lc = 0; lc < 4; lc++)
								Avect[lc].A[j] = 0.0f;
						}

						for (lc = 0; lc < 4; lc++) {
							Svect[lc].V =
									Avect[lc].V
											* vectsV[((col
													+ ImageCropping::cropped_region_offset_x)
													+ lc) % nvects][j].V; // apply the vector

							prow[col + lc] = Svect[lc].A[0] + Svect[lc].A[1]
									+ Svect[lc].A[2] + Svect[lc].A[3]
									+ Svect[lc].A[4] + Svect[lc].A[5]
									+ Svect[lc].A[6]/* + Svect[lc].A[7]*/;
						}
					}
				}
			}
		}

#ifdef XT_TEST_CODE
		{
			short *pTstFrm, *pTstRow;

			// test that we did the right thing...
			for (frame = 0; frame < raw->frames; frame++) {
				pfrm = &(raw->image[frame * raw->frameStride]);
				pTstFrm = &(tstImg[frame * raw->frameStride]);
				for (row = 0; row < raw->rows; row++) {
					prow = pfrm + row * raw->cols;
					pTstRow = pTstFrm + row * raw->cols;
					for (col = 0; col < raw->cols; col++) {
						if (pTstRow[col] > (prow[col]+1) ||
								pTstRow[col] < (prow[col]-1) )
						printf("%s: frame=%d row=%d col=%d   tst=%d img=%d\n",__FUNCTION__,frame,row,col,pTstRow[col],prow[col]);
					}
				}
			}
			free(tstImg);
		}
#endif
	}
	//Dump XT vectors to file
	if (dump_XTvects_to_file) {
		char xtfname[512];
		sprintf(xtfname, "%s/cross_talk_vectors.txt", experimentName);
		FILE* xtfile = fopen(xtfname, "wt");

		if (xtfile != NULL) {
			//write vector length and number of vectors on top
			fprintf(xtfile, "%d\t%d\n", vector_len, nvects);
			//write offsets in single line
			for (int nl = 0; nl < vector_len; nl++)
				fprintf(xtfile, "%d\t", col_offset[nl]);
			fprintf(xtfile, "\n");
			//write vectors tab-separated one line per vector
			for (int vndx = 0; vndx < nvects; vndx++) {
				for (int vn = 0; vn < vector_len; vn++)
					fprintf(xtfile, "%4.6f\t", vects[vndx][vn]);
				fprintf(xtfile, "\n");
			}
			fclose(xtfile);
		}
		dump_XTvects_to_file = 0;
	}
}

ChannelXTCorrection *ImageTransformer::custom_correction_data = NULL;


#define RETRY_INTERVAL 15 // 15 seconds wait time.
#define TOTAL_TIMEOUT 3600 // 1 hr before giving up.



// checks to see if the special lsrowimage.dat file exists in the experiment directory.  If it does,
// this image is used to generate custom channel correction coefficients.  If not, the method silently
// returns (and subsequent analysis uses the default correction).
void ImageTransformer::CalibrateChannelXTCorrection ( const char *exp_dir,const char *filename, bool wait_for_prerun )
{
  // only allow this to be done once
  if ( custom_correction_data != NULL )
    return;

  // LSRowImageProcessor can generate a correction for the 314, but application of the correction is much more
  // difficult than for 316/318, and the expected benefit is not as high, so for now...we're skipping the 314
  if ( ( ChipIdDecoder::GetGlobalChipId() != ChipId316 ) && ( ChipIdDecoder::GetGlobalChipId() != ChipId318 ) && ( ChipIdDecoder::GetGlobalChipId() != ChipId316v2 ) )
    return;

  int len = strlen ( exp_dir ) +strlen ( filename ) + 2;
  char full_fname[len];

  sprintf ( full_fname,"%s/%s",exp_dir,filename );

  if ( wait_for_prerun ) {
    std::string preRun = exp_dir;
    preRun = preRun + "/prerun_0000.dat";
    std::string acq0 = exp_dir;
    acq0 = acq0 + "/acq_0000.dat";

    uint32_t waitTime = RETRY_INTERVAL;
    int32_t timeOut = TOTAL_TIMEOUT;
    //--- Wait up to 3600 seconds for a file to be available
    bool okToProceed = false;
    while ( timeOut > 0 ) {
      //--- do our checkpoint files exist?
      if ( isFile ( preRun.c_str() ) || isFile ( acq0.c_str() ) ) {
        okToProceed = true;
        break;
      }
      fprintf ( stdout, "Waiting to load crosstalk params in %s\n",  full_fname );
      sleep ( waitTime );
      timeOut -= waitTime;
    }
    if ( !okToProceed ) {
      ION_ABORT ( "Couldn't find gateway files for: " + ToStr ( full_fname ) );
    }
    // We got the files we expected so if the xtalk file isn't there then warn.
    if ( !isFile ( full_fname ) ) {
      ION_WARN ( "Didn't find xtalk file: " + ToStr ( full_fname ) );
    }
  }
  LSRowImageProcessor lsrowproc;
  custom_correction_data = lsrowproc.GenerateCorrection ( full_fname );
  if ( custom_correction_data != NULL )
    selected_chip_xt_vectors = custom_correction_data->GetCorrectionDescriptor();

}



// gain correction

float *ImageTransformer::gain_correction = NULL;


void ImageTransformer::GainCorrectImage(RawImage *raw)
{
  for (int row = 0;row < raw->rows;row++)
  {
    for (int col = 0;col < raw->cols;col++)
    {
      float gain = gain_correction[row*raw->cols + col];
      short *prow = raw->image + row*raw->cols + col;

      for (int frame=0;frame < raw->frames;frame++)
      {
        float val = *(prow+frame*raw->frameStride);
        val *= gain;
        if (val > MAX_GAIN_CORRECT) val = MAX_GAIN_CORRECT;

        *(prow+frame*raw->frameStride) = (short)(val);
      }
    }
  }
}

void ImageTransformer::GainCorrectImage(SynchDat *sdat)
{
  for (size_t bIx = 0; bIx < sdat->mChunks.mBins.size(); bIx++) {
    TraceChunk &chunk = sdat->mChunks.mBins[bIx];
    for (size_t row = 0; row < chunk.mHeight; row++) {
      for (size_t col = 0; col < chunk.mWidth; col++){
        float gain = gain_correction[(row+chunk.mRowStart)*sdat->NumCol() + (col+chunk.mColStart)];
        //      size_t numFrames = sdat->NumFrames(row, col);
        size_t numFrames = chunk.mDepth;
        size_t idx = row * chunk.mWidth + col;
        for (size_t frame=0;frame < numFrames;frame++) {
          //        sdat->At(row, col, frame) = std::min(MAX_GAIN_CORRECT, (int)round(sdat->At(row, col, frame) * gain));
          chunk.mData[idx] = std::min(MAX_GAIN_CORRECT, (int)round(chunk.mData[idx]* gain));
          idx += chunk.mFrameStep;
        }
      }
    }
  }
}

float CalculatePixelGain(float *my_trc,float *reference_trc,int min_val_frame, int raw_frames)
{
  float asq = 0.0;
  float axb = 0.0;
  float gain = 1.0f;


  for (int i=0;i < raw_frames;i++)
  {
    if (((i > 3) && (i < min_val_frame-20)) | (i > min_val_frame))
      continue;

    asq += reference_trc[i]*reference_trc[i];
    axb += my_trc[i]*reference_trc[i];
  }
  
  if (axb != 0.0f )
    gain = asq/axb;

  // cap gain to reasonable values
  if (gain < 0.9f)
    gain = 0.9f;

  if (gain > 1.1f)
    gain = 1.1f;

  return gain;
}


// uses a beadfind flow to compute the gain of each pixel
// this can be used as a correction for all future images
void ImageTransformer::GainCalculationFromBeadfind(Mask *mask, RawImage *raw)
{
//  (void)mask;
#if 1
  float avg_trc[raw->frames];
  float my_trc[raw->frames];

  if (gain_correction == NULL)
    gain_correction = new float[raw->rows*raw->cols];

  for (int row = 0;row < raw->rows;row++)
  {
    for (int col = 0;col < raw->cols;col++)
    {
      // don't bother calculating for pinned pixels
      if (mask->Match (col,row,(MaskType) (MaskPinned | MaskExclude | MaskIgnore)))
      {
        gain_correction[row*raw->cols + col] = 1.0f;
        continue;
      }

      int min_val_frame = 0;
      int min_val = 32000;

      bool no_neighbors = true;
      for (int frame = 0;frame < raw->frames;frame++)
      {
        float nnavg = 0.0;
        int avgsum = 0;
        for (int nr=row-3;nr <= row+3;nr++)
        {
          if ((nr >= 0) && (nr < raw->rows))
          {
            short *prow = raw->image + nr*raw->cols + frame*raw->frameStride;
            for (int nc=col-6;nc <= col+6;nc++)
            {
              // skip the column containing the bead being measured
              // as there is significant common-mode gain non-uniformity
              // within the column
              if ((nc >= 0) && (nc < raw->cols) && (nc != col))
              {
                if (!mask->Match (nc,nr,(MaskType) (MaskPinned | MaskExclude | MaskIgnore)))
                {
                  nnavg += prow[nc];
                  avgsum++;
                }
              }
            }

            if (nr == row)
            {
              my_trc[frame] = prow[col];
              if (prow[col] < min_val)
              {
                min_val = prow[col];
                min_val_frame = frame;
              }
            }
          }
        }
        if (avgsum>0)
        {
          avg_trc[frame] = nnavg / avgsum;
          no_neighbors = false;
        }
      }
      if (!no_neighbors)
        gain_correction[row*raw->cols + col] = CalculatePixelGain(my_trc,avg_trc,min_val_frame,raw->frames);
      else
        gain_correction[row*raw->cols + col] = 1.0f;
    }
  }
#endif
}


//@TODO:  Bad to have side effects on every image load from now on
// should be >explicit< pass of gain correction information to image loader
void ImageTransformer::CalculateGainCorrectionFromBeadfindFlow (char *_datDir, bool gain_debug_output)
{
  // calculate gain of each well from the beadfind and use for correction of all images thereafter
  Mask *gainCalcMask;
  std::string datdir = _datDir;
  std::string preBeadFind = datdir + "/beadfind_pre_0003.dat";
  Image bfImg;
  bfImg.SetImgLoadImmediate (false);
  bool loaded = bfImg.LoadRaw (preBeadFind.c_str());
  if (!loaded)
  {
    ION_ABORT ("*Error* - No beadfind file found, did beadfind run? are files transferred?  (" + preBeadFind + ")");
  }

  gainCalcMask = new Mask (bfImg.GetCols(),bfImg.GetRows());

  bfImg.FilterForPinned (gainCalcMask, MaskEmpty, false);
  bfImg.SetMeanOfFramesToZero (1,3);

  //@TODO:  implicit global variable->explicit global variable-> explicit variable
  GainCalculationFromBeadfind (gainCalcMask,bfImg.raw);
  printf ("Computed gain for each pixel using beadind image\n");

  if (gain_debug_output)
  {
    DumpTextGain(bfImg.GetCols(),bfImg.GetRows());
  }

  bfImg.Close();
  delete gainCalcMask;
}

void ImageTransformer::DumpTextGain(int _image_cols, int _image_rows)
{
      FILE *gainfile = fopen ("gainimg.txt","w");

    for (int row=0;row < _image_rows;row++)
    {
      int col;

      for (col=0;col < (_image_cols-1); col++)
        fprintf (gainfile,"%7.5f\t",getPixelGain (row,col,_image_cols));

      fprintf (gainfile,"%7.5f\n",getPixelGain (row,col,_image_cols));
    }
    fclose (gainfile);
}
