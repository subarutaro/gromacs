/*
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Green Red Orange Magenta Azure Cyan Skyblue
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <math.h>
#include <string.h>

#include "sysstuff.h"
#include "physics.h"
#include "typedefs.h"
#include "smalloc.h"
#include "futil.h"
#include "statutil.h"
#include "copyrite.h"
#include "vec.h"
#include "index.h"
#include "macros.h"
#include "gmx_fatal.h"
#include "xvgr.h"
#include "rmpbc.h"
#include "tpxio.h"
#include "nrjac.h"
#include "gmx_ana.h"


static void gyro_eigen(double **gyr,double *eig,double **eigv,int *ord)
{
  int nrot,d;

  jacobi(gyr,DIM,eig,eigv,&nrot);
  /* Order the eigenvalues */
  ord[0] = 0;
  ord[2] = 2;
  for(d=0; d<DIM; d++) {
    if (eig[d] > eig[ord[0]])
      ord[0] = d;
    if (eig[d] < eig[ord[2]])
      ord[2] = d;
  }
  for(d=0; d<DIM; d++) {
    if (ord[0] != d && ord[2] != d)
      ord[1] = d;
  }
}

/* Calculate mean square internal distances (Auhl et al., JCP 119, 12718) */
static void calc_int_dist(double *intd, rvec *x, int i0, int i1)
{
  int ii, jj;
  const int ml = i1 - i0 + 1; /* Number of beads in molecule. */
  int bd; /* Distance between beads */
  double d;

  for (bd = 1; bd < ml; bd++) {
    d = 0.;
    for (ii = i0; ii <= i1 - bd; ii++)
      d += distance2(x[ii], x[ii+bd]);
    d /= ml - bd;
    intd[bd - 1] += d;
  }
}

int gmx_polystat(int argc,char *argv[])
{
  const char *desc[] = {
    "g_polystat plots static properties of polymers as a function of time",
    "and prints the average.[PAR]",
    "By default it determines the average end-to-end distance and radii",
    "of gyration of polymers. It asks for an index group and split this",
    "into molecules. The end-to-end distance is then determined using",
    "the first and the last atom in the index group for each molecules.",
    "For the radius of gyration the total and the three principal components",
    "for the average gyration tensor are written.",
    "With option [TT]-v[tt] the eigenvectors are written.",
    "With option [TT]-pc[tt] also the average eigenvalues of the individual",
    "gyration tensors are written.",
    "With option [TT]-i[tt] the mean square internal distances are",
    "written.[PAR]",
    "With option [TT]-p[tt] the presistence length is determined.",
    "The chosen index group should consist of atoms that are",
    "consecutively bonded in the polymer mainchains.",
    "The presistence length is then determined from the cosine of",
    "the angles between bonds with an index difference that is even,",
    "the odd pairs are not used, because straight polymer backbones",
    "are usually all trans and therefore only every second bond aligns.",
    "The persistence length is defined as number of bonds where",
    "the average cos reaches a value of 1/e. This point is determined",
    "by a linear interpolation of log(<cos>)."
  };
  static bool bMW = TRUE, bPC = FALSE;
  t_pargs pa[] = {
    { "-mw", FALSE, etBOOL, {&bMW},
      "Use the mass weighting for radii of gyration" },
    { "-pc", FALSE, etBOOL, {&bPC},
      "Plot average eigenvalues" }
  };

  t_filenm   fnm[] = {
    { efTPX, NULL, NULL,  ffREAD  },
    { efTRX, "-f", NULL,  ffREAD  },
    { efNDX, NULL, NULL,  ffOPTRD },
    { efXVG, "-o", "polystat",  ffWRITE },
    { efXVG, "-v", "polyvec", ffOPTWR },
    { efXVG, "-p", "persist",  ffOPTWR },
    { efXVG, "-i", "intdist", ffOPTWR }
  };
#define NFILE asize(fnm)

  t_topology *top;
  output_env_t oenv;
  int    ePBC;
  int    isize,*index,nmol,*molind,mol,nat_min=0,nat_max=0;
  char   *grpname;
  int    status;
  real   t;
  rvec   *x,*bond=NULL;
  matrix box;
  int    natoms,i,j,frame,ind0,ind1,a,d,d2,ord[DIM]={0};
  dvec   cm,sum_eig={0,0,0};
  double **gyr,**gyr_all,eig[DIM],**eigv;
  double sum_eed2,sum_eed2_tot,sum_gyro,sum_gyro_tot,sum_pers_tot;
  int    *ninp=NULL;
  double *sum_inp=NULL,pers;
  double *intd, ymax, ymin;
  double mmol,m;
  char   title[STRLEN];
  FILE   *out,*outv,*outp, *outi;
  char *leg[8] = { "end to end", "<R\\sg\\N>",
	 	         "<R\\sg\\N> eig1", "<R\\sg\\N> eig2", "<R\\sg\\N> eig3",
		         "<R\\sg\\N eig1>", "<R\\sg\\N eig2>", "<R\\sg\\N eig3>" };
  char   **legp,buf[STRLEN];

  CopyRight(stderr,argv[0]);
  parse_common_args(&argc,argv,
		    PCA_CAN_VIEW | PCA_CAN_TIME | PCA_TIME_UNIT | PCA_BE_NICE,
		    NFILE,fnm,asize(pa),pa,asize(desc),desc,0,NULL,&oenv);
		    
  snew(top,1);
  ePBC = read_tpx_top(ftp2fn(efTPX,NFILE,fnm),
		      NULL,box,&natoms,NULL,NULL,NULL,top);
  
  fprintf(stderr,"Select a group of polymer mainchain atoms:\n");
  get_index(&top->atoms,ftp2fn_null(efNDX,NFILE,fnm),
            1,&isize,&index,&grpname);

  snew(molind,top->mols.nr+1);
  nmol = 0;
  mol = -1;
  for(i=0; i<isize; i++) {
    if (i == 0 || index[i] >= top->mols.index[mol+1]) {
      molind[nmol++] = i;
      do {
	mol++;
      } while (index[i] >= top->mols.index[mol+1]);
    }
  }
  molind[nmol] = i;
  nat_min = top->atoms.nr;
  nat_max = 0;
  for(mol=0; mol<nmol; mol++) {
    nat_min = min(nat_min,molind[mol+1]-molind[mol]);
    nat_max = max(nat_max,molind[mol+1]-molind[mol]);
  }
  fprintf(stderr,"Group %s consists of %d molecules\n",grpname,nmol);
  fprintf(stderr,"Group size per molecule, min: %d atoms, max %d atoms\n",
	  nat_min,nat_max);

  sprintf(title,"Size of %d polymers",nmol);
  out = xvgropen(opt2fn("-o",NFILE,fnm),title,get_xvgr_tlabel(oenv),"(nm)",
                oenv);
  xvgr_legend(out,bPC ? 8 : 5,leg,oenv);

  if (opt2bSet("-v",NFILE,fnm)) {
    outv = xvgropen(opt2fn("-v",NFILE,fnm),"Principal components",
		    get_xvgr_tlabel(oenv),"(nm)",oenv);
    snew(legp,DIM*DIM);
    for(d=0; d<DIM; d++) {
      for(d2=0; d2<DIM; d2++) {
	sprintf(buf,"eig%d %c",d+1,'x'+d2);
	legp[d*DIM+d2] = strdup(buf);
      }
    }
    xvgr_legend(outv,DIM*DIM,legp,oenv);
  } else {
    outv = NULL;
  }

  if (opt2bSet("-p",NFILE,fnm)) {
    outp = xvgropen(opt2fn("-p",NFILE,fnm),"Persistence length",
		    get_xvgr_tlabel(oenv),"bonds",oenv);
    snew(bond,nat_max-1);
    snew(sum_inp,nat_min/2);
    snew(ninp,nat_min/2);
  } else {
    outp = NULL;
  }

  if (opt2bSet("-i", NFILE, fnm)) {
    outi = xvgropen(opt2fn("-i", NFILE, fnm), "Internal distances",
                    "n", "<R\\S2\\N(n)>/n (nm\\S2\\N)",oenv);
    i = index[molind[1]-1] - index[molind[0]]; /* Length of polymer -1 */
    snew(intd, i);
  } else {
    intd = NULL;
    outi = NULL;
  }

  natoms = read_first_x(oenv,&status,ftp2fn(efTRX,NFILE,fnm),&t,&x,box);

  snew(gyr,DIM);
  snew(gyr_all,DIM);
  snew(eigv,DIM);
  for(d=0; d<DIM; d++) {
    snew(gyr[d],DIM);
    snew(gyr_all[d],DIM);
    snew(eigv[d],DIM);
  }

  frame        = 0;
  sum_eed2_tot = 0;
  sum_gyro_tot = 0;
  sum_pers_tot = 0;
  do {
    rm_pbc(&(top->idef),ePBC,natoms,box,x,x);
    
    sum_eed2 = 0;
    for(d=0; d<DIM; d++)
      clear_dvec(gyr_all[d]);
    
    if (bPC)
      clear_dvec(sum_eig);

    if (outp) {
      for(i=0; i<nat_min/2; i++) {
	sum_inp[i] = 0;
	ninp[i] = 0;
      }
    }

    for(mol=0; mol<nmol; mol++) {
      ind0 = molind[mol];
      ind1 = molind[mol+1];

      /* Determine end to end distance */
      sum_eed2 += distance2(x[index[ind0]],x[index[ind1-1]]);

      /* Determine internal distances */
      if (outi) {
        calc_int_dist(intd, x, index[ind0], index[ind1-1]);
      }

      /* Determine the radius of gyration */
      clear_dvec(cm);
      for(d=0; d<DIM; d++)
	clear_dvec(gyr[d]);
      mmol = 0;

      for(i=ind0; i<ind1; i++) {
	a = index[i];
	if (bMW)
	  m = top->atoms.atom[a].m;
	else
	  m = 1;
	mmol += m;
	for(d=0; d<DIM; d++) {
	  cm[d] += m*x[a][d];
	  for(d2=0; d2<DIM; d2++)
	    gyr[d][d2] += m*x[a][d]*x[a][d2];
	}
      }
      dsvmul(1/mmol,cm,cm);
      for(d=0; d<DIM; d++) {
	for(d2=0; d2<DIM; d2++) {
	  gyr[d][d2] = gyr[d][d2]/mmol - cm[d]*cm[d2];
	  gyr_all[d][d2] += gyr[d][d2];
	}
      }
      if (bPC) {
	gyro_eigen(gyr,eig,eigv,ord);
	for(d=0; d<DIM; d++)
	  sum_eig[d] += eig[ord[d]];
      }
      if (outp) {
	for(i=ind0; i<ind1-1; i++) {
	  rvec_sub(x[index[i+1]],x[index[i]],bond[i-ind0]);
	  unitv(bond[i-ind0],bond[i-ind0]);
	}
	for(i=ind0; i<ind1-1; i++) {
	  for(j=0; (i+j<ind1-1 && j<nat_min/2); j+=2) {
	    sum_inp[j] += iprod(bond[i-ind0],bond[i-ind0+j]);
	    ninp[j]++;
	  }
	}
      }
    }
    sum_eed2 /= nmol;

    sum_gyro = 0;
    for(d=0; d<DIM; d++) {
      for(d2=0; d2<DIM; d2++)
	gyr_all[d][d2] /= nmol;
      sum_gyro += gyr_all[d][d];
    }

    gyro_eigen(gyr_all,eig,eigv,ord);

    fprintf(out,"%10.3f %8.4f %8.4f %8.4f %8.4f %8.4f",
	    t*get_time_factor(oenv),
	    sqrt(sum_eed2),sqrt(sum_gyro),
	    sqrt(eig[ord[0]]),sqrt(eig[ord[1]]),sqrt(eig[ord[2]]));
    if (bPC) {
      for(d=0; d<DIM; d++)
	fprintf(out," %8.4f",sqrt(sum_eig[d]/nmol));
    }
    fprintf(out,"\n");

    if (outv) {
      fprintf(outv,"%10.3f",t*get_time_factor(oenv));
      for(d=0; d<DIM; d++) {
	for(d2=0; d2<DIM; d2++)
	  fprintf(outv," %6.3f",eigv[ord[d]][d2]);
      }
      fprintf(outv,"\n");
    }

    sum_eed2_tot += sum_eed2;
    sum_gyro_tot += sum_gyro;

    if (outp) {
      i = -1;
      for(j=0; j<nat_min/2; j+=2) {
	sum_inp[j] /= ninp[j];
	if (i == -1 && sum_inp[j] <= exp(-1.0))
	  i = j;
      }
      if (i == -1) {
	pers = j;
      } else {
	/* Do linear interpolation on a log scale */
	pers = i - 2
	  + 2*(log(sum_inp[i-2]) + 1)/(log(sum_inp[i-2]) - log(sum_inp[i]));
      }
      fprintf(outp,"%10.3f %8.4f\n",t*get_time_factor(oenv),pers);
      sum_pers_tot += pers;
    }

    frame++;
  } while (read_next_x(oenv,status,&t,natoms,x,box));

  close_trx(status);

  ffclose(out);
  if (outv)
    ffclose(outv);
  if (outp)
    ffclose(outp);

  sum_eed2_tot /= frame;
  sum_gyro_tot /= frame;
  sum_pers_tot /= frame;
  fprintf(stdout,"\nAverage end to end distance: %.3f (nm)\n",
	  sqrt(sum_eed2_tot));
  fprintf(stdout,"\nAverage radius of gyration:  %.3f (nm)\n",
	  sqrt(sum_gyro_tot));
  if (opt2bSet("-p",NFILE,fnm))
    fprintf(stdout,"\nAverage persistence length:  %.2f bonds\n",
	    sum_pers_tot);

  /* Handle printing of internal distances. */
  if (outi) {
    fprintf(outi, "@    xaxes scale Logarithmic\n");
    ymax = -1;
    ymin = 1e300;
    j = index[molind[1]-1] - index[molind[0]]; /* Polymer length -1. */
    for (i = 0; i < j; i++) {
      intd[i] /= (i + 1) * frame * nmol;
      if (intd[i] > ymax)
        ymax = intd[i];
      if (intd[i] < ymin)
        ymin = intd[i];
    }
    xvgr_world(outi, 1, ymin, j, ymax,oenv);
    for (i = 0; i < j; i++) {
      fprintf(outi, "%d  %8.4f\n", i+1, intd[i]);
    }
    ffclose(outi);
  }

  do_view(oenv,opt2fn("-o",NFILE,fnm),"-nxy");
  if (opt2bSet("-v",NFILE,fnm))
    do_view(oenv,opt2fn("-v",NFILE,fnm),"-nxy");
  if (opt2bSet("-p",NFILE,fnm))
    do_view(oenv,opt2fn("-p",NFILE,fnm),"-nxy");
    
  thanx(stderr);
    
  return 0;
}
