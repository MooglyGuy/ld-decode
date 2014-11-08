/* LD decoder prototype, Copyright (C) 2013 Chad Page.  License: LGPL2 */

#include "ld-decoder.h"
#include "deemp.h"

// capture frequency and fundamental NTSC color frequency
//const double CHZ = (1000000.0*(315.0/88.0)*8.0);
//const double FSC = (1000000.0*(315.0/88.0));

bool pulldown_mode = false;
int ofd = 1;
bool image_mode = false;
char *image_base = "FRAME";
bool bw_mode = false;

bool cwide_mode = false;

bool f_oneframe = false;

// NTSC properties
const double freq = 4.0;
const double hlen = 227.5 * freq; 
const int hleni = (int)hlen; 

const double dotclk = (1000000.0*(315.0/88.0)*freq); 

const double dots_usec = dotclk / 1000000.0; 

// values for horizontal timings 
const double line_blanklen = 10.9 * dots_usec;

double irescale = 327.67;
double irebase = 1;
inline uint16_t ire_to_u16(double ire);

// tunables

int linesout = 480;

double brightness = 240;

double black_ire = 7.5;
int black_u16 = ire_to_u16(black_ire);
int white_u16 = ire_to_u16(100); 
bool whiteflag_detect = true;

double nr_y = 4.0;
double nr_c = 0.0;

inline double IRE(double in) 
{
	return (in * 140.0) - 40.0;
}

// XXX:  This is actually YUV.
struct YIQ {
        double y, i, q;

        YIQ(double _y = 0.0, double _i = 0.0, double _q = 0.0) {
                y = _y; i = _i; q = _q;
        };

	YIQ operator*=(double x) {
		YIQ o;

		o.y = this->y * x;
		o.i = this->i * x;
		o.q = this->q * x;

		return o;
	}
	
	YIQ operator+=(YIQ p) {
		YIQ o;

		o.y = this->y + p.y;
		o.i = this->i + p.i;
		o.q = this->q + p.q;

		return o;
	}
};

double clamp(double v, double low, double high)
{
        if (v < low) return low;
        else if (v > high) return high;
        else return v;
}

inline double u16_to_ire(uint16_t level)
{
	if (level == 0) return -100;
	
	return -60 + ((double)(level - irebase) / irescale); 
}

struct RGB {
        double r, g, b;

        void conv(YIQ _y) {
               YIQ t;

		double y = u16_to_ire(_y.y);
		y = (y - black_ire) * (100 / (100 - black_ire)); 

		double i = (_y.i) / irescale;
		double q = (_y.q) / irescale;

                r = y + (1.13983 * q);
                g = y - (0.58060 * q) - (i * 0.39465);
                b = y + (i * 2.032);
#if 0
		double m = brightness / 100;

                r = clamp(r * m, 0, 255);
                g = clamp(g * m, 0, 255);
                b = clamp(b * m, 0, 255);
#else
		double m = brightness * 256 / 100;

                r = clamp(r * m, 0, 65535);
                g = clamp(g * m, 0, 65535);
                b = clamp(b * m, 0, 65535);
#endif
                //cerr << 'y' << y.y << " i" << y.i << " q" << y.q << ' ';
     };
};

inline uint16_t ire_to_u16(double ire)
{
	if (ire <= -60) return 0;
	
	return clamp(((ire + 60) * irescale) + irebase, 1, 65535);
} 

typedef struct cline {
	YIQ p[910];
} cline_t;

int write_locs = -1;

class Comb
{
	protected:
		int linecount;  // total # of lines process - needed to maintain phase
		int curline;    // current line # in frame 

		int framecode;
		int framecount;	

		bool f_oddframe;	// true if frame starts with odd line

		long long scount;	// total # of samples consumed

		int fieldcount;
		int frames_out;	// total # of written frames
	
		uint16_t frame[1820 * 530];

		uint16_t output[744 * 505 * 3];
		uint16_t obuf[744 * 505 * 3];

		uint16_t rawbuffer[3][844 * 505];
		double LPraw[3][844 * 505];
		double K3D[844 * 505];

		cline_t cbuf[525];
		cline_t prevbuf[525];
		Filter *f_i, *f_q;
		Filter *f_wi, *f_wq;

		Filter *f_hpy, *f_hpi, *f_hpq;
		Filter *f_hpvy, *f_hpvi, *f_hpvq;

		void LPFrame(int fnum)
		{
			for (int l = 24; l < 505; l++) {
				for (int h = 32; h < 844; h++) {
					LPraw[fnum][(l * 844) + h - 16] = f_lpf10h.feed(rawbuffer[fnum][(l * 844) + h]);
				}
			}
		}
	
		void Split(int dim) 
		{
			double lp[844 * 505];
			int f = 1;
			
			if (dim < 3) f = 0;
		
			for (int l = 0; l < 24; l++) {
				uint16_t *line = &rawbuffer[f][l * 844];	
					
				for (int h = 4; h < 840; h++) {
					cbuf[l].p[h].y = line[h]; 
					cbuf[l].p[h].i = 0; 
					cbuf[l].p[h].q = 0; 
				}
			}

			for (int l = 24; l < 505; l++) {
				uint16_t *line = &rawbuffer[f][l * 844];	
				bool invertphase = (line[0] == 16384);
				
				// used for 3d only, but don't want to recalc every pel	
				uint16_t *p3line = &rawbuffer[0][l * 844];	
				uint16_t *n3line = &rawbuffer[2][l * 844];	
						
				uint16_t *p2line = &rawbuffer[f][(l - 2) * 844];	
				uint16_t *n2line = &rawbuffer[f][(l + 2) * 844];	
		
				double f3 = 0, f2 = 0;
				double si = 0, sq = 0;
	
				for (int h = 4; h < 840; h++) {
					int phase = h % 4;

					double k;

					double c[3], d[3], v[3];
					double err[3];
						
					int adr = (l * 844) + h;

					if (1 && (dim >= 3)) {

						if (l == 475) cerr << h - 70 << ' ' << line[h] << ' ' << p2line[h] << ' ' << n2line[h] << ' ' << p3line[h] << ' ' << n3line[h] << endl;  

						c[2] = (((p3line[h] + n3line[h]) / 2) - line[h]); 
						d[2] = fabs((p3line[h] - n3line[h]) / 2); 
						d[2] = fabs(((p3line[h] - line[h]) - (n3line[h] - line[h]))); 
						k = fabs(LPraw[1][adr] - LPraw[0][adr]) + fabs(LPraw[1][adr] - LPraw[2][adr]);
						k /= irescale;
						v[2] = clamp(1 - ((k - 0) / 8), 0, 1);
//						if (l == 100) cerr << "T3 " << h << ' ' << c[2] << ' ' << d[2] << ' ' << v[2] << ' ' << k << endl;
					} else {
						c[2] = d[2] = v[2] = 0;
					}

					K3D[adr] = v[2];

					// 2D filtering - can't do the ends
					if (1 && (dim >= 2) && (l >= 2) && (l <= 502)) {
						c[1] = (((p2line[h] + n2line[h]) / 2) - line[h]); 
						d[1] = fabs((p2line[h] - n2line[h]) / 2); 
						d[1] = fabs(((p2line[h] - line[h]) - (n2line[h] - line[h]))); 
						k = fabs(LPraw[1][adr] - LPraw[1][adr - 844]) + fabs(LPraw[1][adr] - LPraw[1][adr + 844]);
						k /= irescale;
						v[1] = clamp(1 - (k / 10), 0, 1);
//						if (l == 100) cerr << "T2 " << h << ' ' << c[1] << ' ' << d[1] << ' ' << v[1] << ' ' << k << endl;
					} else {
						c[1] = d[1] = v[1] = 0;
					}

					// 1D 
					if (1) {
						c[0] = (((line[h + 2] + line[h - 2]) / 2) - line[h]); 
						double x = d[0] - c[0];
						d[0] = fabs((line[h - 2] - line[h + 2]) / 2); 
						d[0] = fabs(((line[h - 2] - line[h]) - (line[h + 2] - line[h]))); 
						v[0] = c[0] ? 1 - clamp((fabs(d[0] / irescale) / 20), 0, 1) : 0.00;
						v[0] = c[0] ? 1 - clamp((fabs(d[0]) / fabs(c[0])) * 1, 0, 1) : 0;
					} else v[0] = 0;
			
					if ((v[1] + v[2]) >= .5) v[0] = 0;

					if ((v[0] + v[1]) > 0) {
						double v12a = 1 - v[2];
						double v12b = v12a / (v[0] + v[1]);	
			
						v[0] *= v12b;
						v[1] *= v12b;
					}

					double vtot = v[0] + v[1] + v[2];
					double cavg = 0, cavg0 = 0, cavg1 = 0,ctot = 0;
					if (0 && ((l == 100) || (l == 50)) && (h % 2)) {
						cerr << "1 " << h - 70 << ' ' << line[h] << ' ' << line[h - 2] << ' ' << line[h + 2] << ' ' << c[0] << ' ' << d[0] << ' ' << v[0] << ' ' << cavg0 << endl;
						cerr << "2 " << h - 70 << ' ' << line[h] << ' ' << p2line[h] << ' ' << n2line[h] << ' ' << c[1] << ' ' << d[1] << ' ' << v[1] << ' ' << cavg1 << endl;
						cerr << "3 " << h - 70 << ' ' << line[h] << ' ' << p3line[h] << ' ' << n3line[h] << ' ' << c[2] << ' ' << d[2] << ' ' << v[2] << ' ' << cavg << endl;
					}
					
					// crude sort
					for (int s1 = 0; s1 < 3; s1++) {
						for (int s2 = 0; s2 < 2; s2++) {
							if (fabs(c[s2]) > fabs(c[s2 + 1])) {
								double ctmp = c[s2 + 1];
								double dtmp = d[s2 + 1];	
								double vtmp = v[s2 + 1];	

								c[s2 + 1] = c[s2];
								c[s2] = ctmp;
								d[s2 + 1] = d[s2];
								d[s2] = dtmp;
								v[s2 + 1] = v[s2];
								v[s2] = vtmp;
							}
						}
					}

					if (vtot <= .01) {
						//v[0] = .5; 
						v[1] = 1; 
						v[2] = 1; 
						vtot = 2;
					}

					double vused = 0;

					v[0] /= vtot;
					v[1] /= vtot;
					v[2] /= vtot;

					cavg0 = cavg = c[0] * v[0];
					vused = v[0];
					cavg += c[1] * v[1]; //(v[1] * (1 - vused));
					cavg1 = cavg;
					vused += v[1]; //* (1 - vused);
					cavg += c[2] * v[2]; // * (1 - vused));
					vused += (v[2] * (1 - vused));

					if (0 && ((l == 100) || (l == 50)) && (h % 2)) {
						cerr << 'a' << h - 70 << ' ' << line[h] << ' ' << c[0] << ' ' << d[0] << ' ' << v[0] << ' ' << cavg0 << endl;
						cerr << 'b' << h - 70 << ' ' << line[h] << ' ' << c[1] << ' ' << d[1] << ' ' << v[1] << ' ' << cavg1 << endl;
						cerr << 'c' << h - 70 << ' ' << line[h] << ' ' << c[2] << ' ' << d[2] << ' ' << v[2] << ' ' << cavg << endl;
					}

					cavg /= 2;
					if (!invertphase) cavg = -cavg;

					switch (phase) {
						case 0: si = cavg; break;
						case 1: sq = -cavg; break;
						case 2: si = -cavg; break;
						case 3: sq = cavg; break;
						default: break;
					}

					cbuf[l].p[h].y = line[h]; 
					cbuf[l].p[h].i = si;  
					cbuf[l].p[h].q = sq; 
				}

				for (int h = 4; 1 && h < 840; h++) {
					if (cwide_mode) {
						cbuf[l].p[h - 5].i = bw_mode ? 0 : f_wi->feed(cbuf[l].p[h].i); 
						cbuf[l].p[h - 5].q = bw_mode ? 0 : f_wq->feed(cbuf[l].p[h].q); 
					} else {
						cbuf[l].p[h - 8].i = bw_mode ? 0 : f_i->feed(cbuf[l].p[h].i); 
						cbuf[l].p[h - 8].q = bw_mode ? 0 : f_q->feed(cbuf[l].p[h].q); 
					}	
				}	

			}
		}
					
		void DoCNR(int fnum = 0) {
			if (nr_c < 0) return;

			// part 1:  do horizontal 
			for (int l = 24; l < 505; l++) {
				YIQ hpline[844];
				cline_t *input = &cbuf[l];

				for (int h = 70; h < 752 + 70; h++) {
					YIQ y = input->p[h]; 

					hpline[h].i = f_hpi->feed(y.i);
					hpline[h].q = f_hpq->feed(y.q);
				}

				for (int h = 70; h < 744 + 70; h++) {
					YIQ a = hpline[h + 8];

					if (fabs(a.i) < nr_c) {
						double hpm = (a.i / nr_c);
						a.i *= (1 - fabs(hpm * hpm * hpm));
						//a.i -= hpm;
						input->p[h].i -= a.i;
					}
					
					if (fabs(a.q) < nr_c) {
						double hpm = (a.q / nr_c);
						a.q *= (1 - fabs(hpm * hpm * hpm));
						//a.q -= hpm;
						input->p[h].q -= a.q;
					}
				}
			}
		}
		
		void DoYNR() {
			int firstline = (linesout == 505) ? 0 : 24;
			if (nr_y < 0) return;

			// part 1:  do horizontal 
			for (int l = firstline; l < 505; l++) {
				YIQ hpline[844];
				cline_t *input = &cbuf[l];

				for (int h = 70; h < 752 + 70; h++) {
					hpline[h].y = f_hpy->feed(input->p[h].y);
				}

				for (int h = 70; h < 744 + 70; h++) {
					YIQ a = hpline[h + 8];

					if (fabs(a.y) < nr_y) {
						double hpm = (a.y / nr_y);
						a.y *= (1 - fabs(hpm * hpm * hpm));
						input->p[h].y -= a.y;
					}
				}
			}
		}
		
		uint32_t ReadPhillipsCode(uint16_t *line) {
			int first_bit = -1 ;// (int)100 - (1.0 * dots_usec);
			const double bitlen = 2.0 * dots_usec;
			uint32_t out = 0;

			// find first bit
		
			for (int i = 70; (first_bit == -1) && (i < 140); i++) {
				if (u16_to_ire(line[i]) > 90) {
					first_bit = i - (1.0 * dots_usec); 
				}
//				cerr << i << ' ' << line[i] << ' ' << u16_to_ire(line[i]) << ' ' << first_bit << endl;
			}
			if (first_bit < 0) return 0;

			for (int i = 0; i < 24; i++) {
				double val = 0;
	
			//	cerr << dots_usec << ' ' << (int)(first_bit + (bitlen * i) + dots_usec) << ' ' << (int)(first_bit + (bitlen * (i + 1))) << endl;	
				for (int h = (int)(first_bit + (bitlen * i) + dots_usec); h < (int)(first_bit + (bitlen * (i + 1))); h++) {
//					cerr << h << ' ' << line[h] << ' ' << endl;
					val += u16_to_ire(line[h]); 
				}

//				cerr << "bit " << 23 - i << " " << val / dots_usec << ' ' << hex << out << dec << endl;	
				if ((val / dots_usec) > 50) {
					out |= (1 << (23 - i));
				} 
			}
			cerr << "P " << curline << ' ' << hex << out << dec << endl;			

			return out;
		}

	public:
		Comb() {
			fieldcount = curline = linecount = -1;
			framecode = framecount = frames_out = 0;

			scount = 0;

			f_oddframe = false;	
	
			f_i = new Filter(f_colorlp4);
			f_q = new Filter(f_colorlp4);
			
			f_wi = new Filter(f_colorwlp4);
			f_wq = new Filter(f_colorwlp4);

			f_hpy = new Filter(f_nr);
			f_hpi = new Filter(f_nrc);
			f_hpq = new Filter(f_nrc);
			
			f_hpvy = new Filter(f_nr);
			f_hpvi = new Filter(f_nrc);
			f_hpvq = new Filter(f_nrc);

			memset(output, 0, sizeof(output));
		}

		void WriteFrame(uint16_t *obuf, int fnum = 0) {
			cerr << "WR" << fnum << endl;
			if (!image_mode) {
				write(ofd, obuf, (744 * linesout * 3) * 2);
			} else {
				char ofname[512];

				sprintf(ofname, "%s%d.rgb", image_base, fnum); 
				cerr << "W " << ofname << endl;
				ofd = open(ofname, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IROTH);
				write(ofd, obuf, (744 * linesout * 3) * 2);
				close(ofd);
			}
			if (f_oneframe) exit(0);
		}
		
		// buffer: 844x505 uint16_t array
		void Process(uint16_t *buffer, int dim = 2)
		{
			int firstline = (linesout == 505) ? 1 : 26;
			int f = (dim == 3) ? 1 : 0;

			cerr << "P " << f << ' ' << dim << endl;

			memcpy(rawbuffer[2], rawbuffer[1], (844 * 505 * 2));
			memcpy(rawbuffer[1], rawbuffer[0], (844 * 505 * 2));
			memcpy(rawbuffer[0], buffer, (844 * 505 * 2));
			
			memcpy(LPraw[2], LPraw[1], (844 * 505 * sizeof(double)));
			memcpy(LPraw[1], LPraw[0], (844 * 505 * sizeof(double)));
		
			memcpy(prevbuf, cbuf, sizeof(cbuf));
	
			LPFrame(0);
		
			if ((dim == 3) && (framecount < 2)) {
				framecount++;
				return;
			} 
	
			Split(dim); 

			DoCNR(0);	
			
			// remove color data from baseband (Y)	
			for (int l = firstline; l < 505; l++) {
				bool invertphase = (rawbuffer[f][l * 844] == 16384);

				for (int h = 0; h < 760; h++) {
					double comp;	
					int phase = h % 4;

					YIQ y = cbuf[l].p[h + 70];

					switch (phase) {
						case 0: comp = y.i; break;
						case 1: comp = -y.q; break;
						case 2: comp = -y.i; break;
						case 3: comp = y.q; break;
						default: break;
					}

					if (invertphase) comp = -comp;
					y.y += comp;

					cbuf[l].p[h + 70] = y;
				}
			}
			
			DoYNR();
		
			// YIQ (YUV?) -> RGB conversion	
			for (int l = firstline; l < 505; l++) {
				uint16_t *line_output = &output[(744 * 3 * (l - firstline))];
				int o = 0;
				for (int h = 0; h < 752; h++) {
					RGB r;
					YIQ yiq = cbuf[l].p[h + 74];

					// merge code - works but causes bad artifacs 
					if (0 && framecount > 2) {
	
						double k = K3D[(l * 844) + (h + 70)] * .5; 

						// XXX: fix operator override

						yiq.y *= (1 - k);
						yiq.i *= (1 - k);
						yiq.q *= (1 - k);
						yiq.y += (prevbuf[l].p[h + 70].y * k);
						yiq.i += (prevbuf[l].p[h + 70].i * k);
						yiq.q += (prevbuf[l].p[h + 70].q * k);
					}
	
					// for debug: display difference areas	
					if (0) {
						int adr = (l * 844) + (h + 70);
               	                                 double k = fabs(LPraw[1][adr] - LPraw[0][adr]) + fabs(LPraw[1][adr] - LPraw[2][adr]);
//            	                                   k /= irescale;
//						cerr << k << ' ' << yiq.y << endl;
						yiq.y = k;
						yiq.i = yiq.q = 0;
					}
	
					r.conv(yiq);
	
      	                                if (0 && l == 475) {
                                               double y = u16_to_ire(yiq.y);
                                               double i = (yiq.i) * (160.0 / 65533.0);
                                               double q = (yiq.q) * (160.0 / 65533.0);

                                               double m = ctor(q, i);
                                               double a = atan2(q, i);

                                               a *= (180 / M_PIl);
                                               if (a < 0) a += 360;

						double k = K3D[(l * 844) + (h + 70)] * .5; 
                                               cerr << h << ' ' << k << ' ' << y << ' ' << i << ' ' << q << ' ' << m << ' ' << a << ' '; 
                                               cerr << r.r << ' ' << r.g << ' ' << r.b << endl;
                                       }

					line_output[o++] = (uint16_t)(r.r); 
					line_output[o++] = (uint16_t)(r.g); 
					line_output[o++] = (uint16_t)(r.b); 
				}
			}

			PostProcess(f);
			framecount++;

			return;
		}

		int PostProcess(int fnum) {
			int fstart = -1;

			if (!pulldown_mode) {
				fstart = 0;
			} else if (f_oddframe) {
				for (int i = 0; i < linesout; i += 2) {
					memcpy(&obuf[744 * 3 * i], &output[744 * 3 * i], 744 * 3 * 2); 
				}
				WriteFrame(obuf, framecode);
				f_oddframe = false;		
			}

			for (int line = 4; line <= 5; line++) {
				int wc = 0;
				for (int i = 0; i < 700; i++) {
					if (rawbuffer[fnum][(844 * line) + i] > 45000) wc++;
				} 
				if (wc > 500) {
					fstart = (line % 2); 
				}
				//cerr << "PW" << fnum << ' ' << line << ' ' << wc << ' ' << fieldcount << endl;
			}

			for (int line = 16; line < 20; line++) {
				int new_framecode = ReadPhillipsCode(&rawbuffer[fnum][line * 844]); // - 0xf80000;
				int fca = new_framecode & 0xf80000;

				//cerr << "c" << line << ' ' << hex << ' ' <<  new_framecode << ' ' << fca << ' ' << dec << endl;

				if (((new_framecode & 0xf00000) == 0xf00000) && (new_framecode < 0xff0000)) {
					int ofstart = fstart;

					framecode = new_framecode & 0x0f;
					framecode += ((new_framecode & 0x000f0) >> 4) * 10;
					framecode += ((new_framecode & 0x00f00) >> 8) * 100;
					framecode += ((new_framecode & 0x0f000) >> 12) * 1000;
					framecode += ((new_framecode & 0xf0000) >> 16) * 10000;

					if (framecode > 80000) framecode -= 80000;
	
					cerr << "frame " << framecode << endl;
	
					fstart = (line % 2); 
					if ((ofstart >= 0) && (fstart != ofstart)) {
						cerr << "MISMATCH\n";
					}
				}
			}

			cerr << "FR " << framecount << ' ' << fstart << endl;
			if (!pulldown_mode || (fstart == 0)) {
				WriteFrame(output, framecode);
			} else if (fstart == 1) {
				for (int i = 1; i < linesout; i += 2) {
					memcpy(&obuf[744 * 3 * i], &output[744 * 3 * i], 744 * 3 * 2); 
				}
				f_oddframe = true;
			}

			return 0;
		}
};
	
Comb comb;

void usage()
{
	cerr << "comb: " << endl;
	cerr << "-i [filename] : input filename (default: stdin)\n";
	cerr << "-o [filename] : output filename/base (default: stdout/frame)\n";
	cerr << "-f : use separate file for each frame\n";
	cerr << "-p : use white flag/frame # for pulldown\n";	
	cerr << "-h : this\n";	
}

int main(int argc, char *argv[])
{
	int rv = 0, fd = 0;
	long long dlen = -1, tproc = 0;
	unsigned short inbuf[844 * 525 * 2];
	unsigned char *cinbuf = (unsigned char *)inbuf;
	int c;

	int dim = 2;

	char out_filename[256] = "";

	cerr << std::setprecision(10);
	cerr << argc << endl;
	cerr << strncmp(argv[1], "-", 1) << endl;

	opterr = 0;
	
	while ((c = getopt(argc, argv, "Owvd:Bb:I:w:i:o:fphn:N:")) != -1) {
		switch (c) {
			case 'd':
				sscanf(optarg, "%d", &dim);
				break;
			case 'O':
				f_oneframe = true;
				break;
			case 'v':
				linesout = 505;
				break;
			case 'B':
				bw_mode = true;
				break;
			case 'w':
				cwide_mode = true;
				break;
			case 'b':
				sscanf(optarg, "%lf", &brightness);
				break;
			case 'I':
				sscanf(optarg, "%lf", &black_ire);
				break;
			case 'n':
				sscanf(optarg, "%lf", &nr_y);
				break;
			case 'N':
				sscanf(optarg, "%lf", &nr_c);
				break;
			case 'h':
				usage();
				return 0;
			case 'f':
				image_mode = true;	
				break;
			case 'p':
				pulldown_mode = true;	
				break;
			case 'i':
				fd = open(optarg, O_RDONLY);
				break;
			case 'o':
				image_base = (char *)malloc(strlen(optarg) + 1);
				strncpy(image_base, optarg, strlen(optarg));
				break;
			default:
				return -1;
		} 
	} 

	black_u16 = ire_to_u16(black_ire);
	cerr << ' ' << black_u16 << endl;

	nr_y = nr_y * irescale;
	nr_c = nr_c * irescale;

	if (!image_mode && strlen(out_filename)) {
		ofd = open(image_base, O_WRONLY | O_CREAT);
	}

	cout << std::setprecision(8);

	int bufsize = 844 * 505 * 2;

	rv = read(fd, inbuf, bufsize);
	while ((rv > 0) && (rv < bufsize)) {
		int rv2 = read(fd, &cinbuf[rv], bufsize - rv);
		if (rv2 <= 0) exit(0);
		rv += rv2;
	}

	while (rv == bufsize && ((tproc < dlen) || (dlen < 0))) {
		comb.Process(inbuf, dim);
	
		rv = read(fd, inbuf, bufsize);
		while ((rv > 0) && (rv < bufsize)) {
			int rv2 = read(fd, &cinbuf[rv], bufsize - rv);
			if (rv2 <= 0) exit(0);
			rv += rv2;
		}
	}

	return 0;
}

