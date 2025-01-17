/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2018 Ingo Weyrich (heckflosse67@gmx.de)
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

//
//   Adaptive Homogeneity-Directed interpolation is based on
//   the work of Keigo Hirakawa, Thomas Parks, and Paul Lee.
//   Optimized for speed and reduced memory usage 2018 Ingo Weyrich
//

#include <cmath>
#include <climits>
#include "bayerhelper.h"
#include "LUT.h"
#include "librtprocess.h"
#include "opthelper.h"
#include "rt_math.h"
#include "median.h"
#include "StopWatch.h"

#define TS 144

using namespace librtprocess;

rpError ahd_demosaic(int width, int height, const float * const *rawData, float **red, float **green, float **blue, const unsigned cfarray[2][2], const float rgb_cam[3][4], const std::function<bool(double)> &setProgCancel)
{
    BENCHFUN

    if (!validateBayerCfa(3, cfarray)) {
        return RP_WRONG_CFA;
    }

    rpError rc = RP_NO_ERROR;

    constexpr int dir[4] = { -1, 1, -TS, TS };
    float xyz_cam[3][3];
    LUTf cbrt(65536);

    constexpr double xyz_rgb[3][3] = {        /* XYZ from RGB */
        { 0.412453, 0.357580, 0.180423 },
        { 0.212671, 0.715160, 0.072169 },
        { 0.019334, 0.119193, 0.950227 }
    };

    constexpr float d65_white[3] = { 0.950456, 1, 1.088754 };

    double progress = 0.0;
    setProgCancel(progress);

    for (int i = 0; i < 65536; i++) {
        const double r = i / 65535.0;
        cbrt[i] = r > 0.008856 ? std::cbrt(r) : 7.787 * r + 16 / 116.0;
    }

    for (int i = 0; i < 3; i++)
        for (unsigned int j = 0; j < 3; j++) {
            xyz_cam[i][j] = 0;
            for (int k = 0; k < 3; k++) {
                xyz_cam[i][j] += xyz_rgb[i][k] * rgb_cam[k][j] / d65_white[i];
            }
        }

    rc = bayerborder_demosaic(width, height, 5, rawData, red, green, blue, cfarray);

#ifdef _OPENMP
#pragma omp parallel
#endif
{
    int progresscounter = 0;
    float *buffer = new (std::nothrow) float[13 * TS * TS]; /* 1053 kB per core */
#ifdef _OPENMP
    #pragma omp critical
#endif
    {
        if (!buffer) {
            rc = RP_MEMORY_ERROR;
        }
    }
#ifdef _OPENMP
    #pragma omp barrier
#endif
    if (!rc) {
        auto rgb  = (float(*)[TS][TS][3]) buffer;
        auto lab  = (float(*)[TS][TS][3])(buffer + 6 * TS * TS);
        auto homo = (uint16_t(*)[TS][TS])(buffer + 12 * TS * TS);

#ifdef _OPENMP
#if defined(_MSC_VER) && defined(WIN32) 
	#pragma omp parallel for
#else
        #pragma omp for collapse(2) schedule(dynamic) nowait
#endif
#endif
        for (int top = 2; top < height - 5; top += TS - 6) {
            for (int left = 2; left < width - 5; left += TS - 6) {
                //  Interpolate green horizontally and vertically:
                for (int row = top; row < top + TS && row < height - 2; row++) {
            for (int col = left + (fc(cfarray, row, left) & 1); col < std::min(left + TS, width - 2); col += 2) {
                        auto pix = &rawData[row][col];
                        float val0 = 0.25f * ((pix[-1] + pix[0] + pix[1]) * 2
                                      - pix[-2] - pix[2]) ;
                        rgb[0][row - top][col - left][1] = median(val0, pix[-1], pix[1]);
                        float val1 = 0.25f * ((pix[-width] + pix[0] + pix[width]) * 2
                                      - pix[-2 * width] - pix[2 * width]) ;
                        rgb[1][row - top][col - left][1] = median(val1, pix[-width], pix[width]);
                    }
                }

                //  Interpolate red and blue, and convert to CIELab:
                for (int d = 0; d < 2; d++)
                    for (int row = top + 1; row < top + TS - 1 && row < height - 3; row++) {
                int cng = fc(cfarray, row + 1, fc(cfarray, row + 1, 0) & 1);
                        for (int col = left + 1; col < std::min(left + TS - 1, width - 3); col++) {
                            auto pix = &rawData[row][col];
                            auto rix = &rgb[d][row - top][col - left];
                            auto lix = lab[d][row - top][col - left];
                if (fc(cfarray, row, col) == 1) {
                                rix[0][2 - cng] = CLIP(pix[0] + (0.5f * (pix[-1] + pix[1]
                                                           - rix[-1][1] - rix[1][1] ) ));
                                rix[0][cng] = CLIP(pix[0] + (0.5f * (pix[-width] + pix[width]
                                                           - rix[-TS][1] - rix[TS][1])));
                                rix[0][1] = pix[0];
                            } else {
                                rix[0][cng] = CLIP(rix[0][1] + (0.25f * (pix[-width - 1] + pix[-width + 1]
                                                            + pix[+width - 1] + pix[+width + 1]
                                                            - rix[-TS - 1][1] - rix[-TS + 1][1]
                                                            - rix[+TS - 1][1] - rix[+TS + 1][1])));
                                rix[0][2 - cng] = pix[0];
                            }
                            float xyz[3] = {};
                            
                            for(unsigned int c = 0; c < 3; ++c) {
                                xyz[0] += xyz_cam[0][c] * rix[0][c];
                                xyz[1] += xyz_cam[1][c] * rix[0][c];
                                xyz[2] += xyz_cam[2][c] * rix[0][c];
                            }

                            xyz[0] = cbrt[xyz[0]];
                            xyz[1] = cbrt[xyz[1]];
                            xyz[2] = cbrt[xyz[2]];

                            lix[0] = 116.f * xyz[1] - 16.f;
                            lix[1] = 500.f * (xyz[0] - xyz[1]);
                            lix[2] = 200.f * (xyz[1] - xyz[2]);
                        }
                    }

                //  Build homogeneity maps from the CIELab images:

                for (int row = top + 2; row < top + TS - 2 && row < height - 4; row++) {
                    int tr = row - top;
                    float ldiff[2][4], abdiff[2][4];

                    for (int col = left + 2, tc = 2; col < left + TS - 2 && col < width - 4; col++, tc++) {
                        for (int d = 0; d < 2; d++) {
                            auto lix = &lab[d][tr][tc];

                            for (int i = 0; i < 4; i++) {
                                ldiff[d][i] = std::fabs(lix[0][0] - lix[dir[i]][0]);
                                abdiff[d][i] = SQR(lix[0][1] - lix[dir[i]][1])
                                               + SQR(lix[0][2] - lix[dir[i]][2]);
                            }
                        }

                        float leps = std::min(std::max(ldiff[0][0], ldiff[0][1]),
                                         std::max(ldiff[1][2], ldiff[1][3]));
                        float abeps = std::min(std::max(abdiff[0][0], abdiff[0][1]),
                                          std::max(abdiff[1][2], abdiff[1][3]));

                        for (int d = 0; d < 2; d++) {
                            homo[d][tr][tc] = 0;
                            for (int i = 0; i < 4; i++) {
                                homo[d][tr][tc] += (ldiff[d][i] <= leps) * (abdiff[d][i] <= abeps);
                            }
                        }
                    }
                }

                //  Combine the most homogenous pixels for the final result:
                for (int row = top + 3; row < top + TS - 3 && row < height - 5; row++) {
                    int tr = row - top;

                    for (int col = left + 3, tc = 3; col < std::min(left + TS - 3, width - 5); col++, tc++) {
                        uint16_t hm0 = 0, hm1 = 0;
                        for (int i = tr - 1; i <= tr + 1; i++)
                            for (int j = tc - 1; j <= tc + 1; j++) {
                                hm0 += homo[0][i][j];
                                hm1 += homo[1][i][j];
                            }

                        if (hm0 != hm1) {
                            int ldir = hm1 > hm0;
                            red[row][col] = rgb[ldir][tr][tc][0];
                            green[row][col] = rgb[ldir][tr][tc][1];
                            blue[row][col] = rgb[ldir][tr][tc][2];
                        } else {
                            red[row][col] = 0.5f * (rgb[0][tr][tc][0] + rgb[1][tr][tc][0]);
                            green[row][col] = 0.5f * (rgb[0][tr][tc][1] + rgb[1][tr][tc][1]);
                            blue[row][col] = 0.5f * (rgb[0][tr][tc][2] + rgb[1][tr][tc][2]);
                        }
                    }
                }


                progresscounter++;
                if(progresscounter % 32 == 0) {
#ifdef _OPENMP
                    #pragma omp critical (ahdprogress)
#endif
                    {
                        progress += 32.0 * SQR(TS - 6) / (height * width);
                        progress = std::min(progress, 1.0);
                        setProgCancel(progress);
                    }
                }

            }
        }
    }
    delete [] buffer;
}

    setProgCancel(1.0);

    return rc;
}
#undef TS




