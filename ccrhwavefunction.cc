#include "ccrhwavefunction.h"
#include "globals.h"
#include "hamiltonian.h"
#include <boost/shared_ptr.hpp>
#include <libqt/qt.h>
#include <libciomr/libciomr.h>
#include <cmath>
#include <libpsio/psio.h>

namespace psi { namespace ugacc {

CCRHWavefunction::CCRHWavefunction(boost::shared_ptr<Wavefunction> reference, boost::shared_ptr<Hamiltonian> H, Options &options, boost::shared_ptr<PSIO> psio) : Wavefunction(options, psio)
{
  wfn_ = options.get_str("WFN");
  convergence_ = options.get_double("R_CONVERGENCE");
  maxiter_ = options.get_int("MAXITER");
  do_diis_ = options.get_bool("DIIS");
  ooc_ = options.get_bool("OOC");
  if(options.get_str("DERTYPE") == "NONE") dertype_ = 0;
  else if(options.get_str("DERTYPE") == "FIRST") dertype_ = 1;

  set_reference_wavefunction(reference);
  copy(reference);

  int nfrzv = 0;
  no_ = nv_ = 0;
  for(int i=0; i < nirrep_; i++) {
    no_ += doccpi_[i] - frzcpi_[i];
    nv_ += nmopi_[i] - doccpi_[i] - frzvpi_[i];
    nfrzv += frzvpi_[i];
  }
  char ** labels = molecule_->irrep_labels();

  outfile->Printf("\n\tReference Wfn Parameters:\n");
  outfile->Printf("\t---------------------------\n");
  outfile->Printf("\tNumber of irreps        = %d\n", nirrep_);
  outfile->Printf("\tNumber of MOs           = %d\n", nmo_);
  outfile->Printf("\tNumber of active MOs    = %d\n", no_+nv_);
  outfile->Printf("\tNumber of active occ    = %d\n", no_);
  outfile->Printf("\tNumber of active vir    = %d\n", nv_);
  outfile->Printf("\tNumber of frozen occ    = %d\n", nfrzc_);
  outfile->Printf("\tNumber of frozen vir    = %d\n\n", nfrzv);
  outfile->Printf("\tLabel\t# MOs\t# FZDC\t# DOCC\t# VIRT\t# FZVR\n");
  outfile->Printf("\t-----\t-----\t------\t------\t------\t------\n");
  for(int i=0; i < nirrep_; i++) {
      outfile->Printf("\t %s\t   %d\t    %d\t    %d\t    %d\t    %d\n",
              labels[i],nmopi_[i],frzcpi_[i],doccpi_[i],nmopi_[i]-doccpi_[i],frzvpi_[i]);
    }
  outfile->Printf("\n\tNuclear Repulsion Energy    = %20.15f\n", molecule_->nuclear_repulsion_energy());
  outfile->Printf( "\tFrozen Core Energy          = %20.15f\n", reference->efzc());
  outfile->Printf( "\tTotal SCF Energy (ref)      = %20.15f\n", reference_wavefunction_->reference_energy());

  for(int i=0; i < nirrep_; i++) free(labels[i]);
  free(labels);

  H_ = H;

  // Prepare energy denominators
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;

  D1_ = block_matrix(no,nv);
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      D1_[i][a] = fock[i][i] - fock[a+no][a+no];

  D2_ = init_4d_array(no,no,nv,nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          D2_[i][j][a][b] = fock[i][i] + fock[j][j] - fock[a+no][a+no] - fock[b+no][b+no];

  // initial guess amplitudes
  t1_ = block_matrix(no,nv);
  t1old_ = block_matrix(no,nv);
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      t1_[i][a] = fock[i][a+no]/D1_[i][a];

  t2_ = init_4d_array(no,no,nv,nv);
  t2old_ = init_4d_array(no,no,nv,nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          t2_[i][j][a][b] = ints[i][j][a+no][b+no]/D2_[i][j][a][b];

  tau_ = init_4d_array(no,no,nv,nv);
  ttau_ = init_4d_array(no,no,nv,nv);

  build_tau();

  Fvv_ = block_matrix(nv,nv);
  Foo_ = block_matrix(no,no);
  Fov_ = block_matrix(no,nv);

  Woooo_ = init_4d_array(no,no,no,no);
  Wovov_ = init_4d_array(no,nv,no,nv);
  Wovvo_ = init_4d_array(no,nv,nv,no);
}

CCWavefunction::~CCWavefunction()
{
  int no = no_;
  int nv = nv_;

  free_block(D1_);
  free_4d_array(D2_, no, no, nv);
  free_block(t1_);
  free_block(t1old_);
  free_4d_array(t2_, no, no, nv);
  free_4d_array(t2old_, no, no, nv);
  free_4d_array(tau_, no, no, nv);
  free_4d_array(ttau_, no, no, nv);
  free_block(Fvv_);
  free_block(Foo_);
  free_block(Fov_);
  free_4d_array(Woooo_, no, no, no);
  free_4d_array(Wovov_, no, nv, no);
  free_4d_array(Wovvo_, no, nv, nv);

  free_block(t1s_);
  free_4d_array(t2s_, no, no, nv);

  if(wfn() == "CCSD_T" && ooc() == false)
    free_6d_array(t3_, no, no, no, nv, nv);
}

double CCWavefunction::compute_energy() { 
  double eref, emp2, eccsd, et;
  eref = reference_energy();

  outfile->Printf("\n\tThe Coupled-Cluster Iteration:\n");
  outfile->Printf(  "\t---------------------------------------------------\n");
  outfile->Printf(  "\t Iter   Correlation Energy   T1 Norm      RMS   \n");
  outfile->Printf(  "\t---------------------------------------------------\n");
  outfile->Printf(  "\t  %3d  %20.15f\n", 0, emp2 = energy());

  double rms = 0.0;
  for(int iter=1; iter <= maxiter(); iter++) {
    amp_save("T");
    build_F();
    build_W();
    build_t1();
    build_t2();
    rms = increment_amps("T");
    if(rms < convergence()) break;
    if(do_diis()) diis(iter, "T");
    build_tau();
    outfile->Printf(  "\t  %3d  %20.15f  %8.6f  %8.6e\n",iter, eccsd = energy(), t1norm(), rms);
  }

  build_tau();
  build_tstar();

  if(rms >= convergence())
    throw PSIEXCEPTION("Computation has not converged.");

  double etotal = eccsd + eref;
  outfile->Printf(  "\n\tMP2 Correlation Energy     = %20.14f\n", emp2);
  outfile->Printf(  "\tMP2 Total Energy           = %20.14f\n", emp2 + eref);
  outfile->Printf(  "\tCCSD Correlation Energy    = %20.14f\n", eccsd);
  outfile->Printf(  "\tCCSD Total Energy          = %20.14f\n", eccsd + eref);
  if(wfn() == "CCSD_T") {
    if(ooc()) {
      outfile->Printf("\t(T) Correction             = %20.14f (ooc)\n", et = tcorr_ooc());
      outfile->Printf("\t(T) Correction             = %20.14f (TJL)\n", tcorr_ooc_TJL());
    }
    else outfile->Printf("\t(T) Correction             = %20.14f\n", et = tcorr());
    outfile->Printf("\tCCSD(T) Correlation Energy = %20.14f\n", eccsd + et);
    outfile->Printf("\tCCSD(T) Total Energy       = %20.14f\n", eref + eccsd + et);
    etotal += et;
  }

  return etotal;
}

double CCWavefunction::energy()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****L = H_->L_;
  double **t1 = t1_;
  double ****tau = tau_;

  double one_energy=0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      one_energy = 2 * fock[i][a+no] * t1[i][a];

  double two_energy=0;
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          two_energy += tau[i][j][a][b]*L[i][j][a+no][b+no];

  return one_energy + two_energy;
}

void CCWavefunction::build_tau()
{
  int no = no_;
  int nv = nv_;
  double **t1 = t1_;
  double ****t2 = t2_;

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          tau_[i][j][a][b] = t2[i][j][a][b] + t1[i][a] * t1[j][b];
          ttau_[i][j][a][b] = t2[i][j][a][b] + 0.5 * t1[i][a] * t1[j][b];
        }
}

void CCWavefunction::amp_save()
{
  double ****t2tmp = t2_;
  t2_ = t2old_;
  t2old_ = t2tmp;

  double **t1tmp = t1_;
  t1_ = t1old_;
  t1old_ = t1tmp;
}

void CCWavefunction::build_F()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****L = H_->L_;
  double **t1 = t1old_;
  double ****ttau = ttau_;

  for(int a=0; a < nv; a++)
    for(int e=0; e < nv; e++) {
      double value = fock[a+no][e+no];
      for(int m=0; m < no; m++) {
        value -= 0.5*fock[m][e+no]*t1[m][a];
        for(int f=0; f < nv; f++) {
          value += t1[m][f]*L[m][a+no][f+no][e+no];
          for(int n=0; n < no; n++)
            value -= ttau[m][n][a][f]*L[m][n][e+no][f+no];
        }
      }
      Fvv_[a][e] = value;
    }

  for(int m=0; m < no; m++)
    for(int i=0; i < no; i++) {
      double value = fock[m][i];
      for(int e=0; e < nv; e++) {
        value += 0.5*t1[i][e]*fock[m][e+no];
        for(int n=0; n < no; n++) {
          value += t1[n][e]*L[m][n][i][e+no];
          for(int f=0; f < nv; f++)
            value += ttau[i][n][e][f]*L[m][n][e+no][f+no];
        }
      }
      Foo_[m][i] = value;
    }

  for(int m=0; m < no; m++)
    for(int e=0; e < nv; e++) {
      double value = fock[m][e+no];
      for(int n=0; n < no; n++)
        for(int f=0; f < nv; f++)
          value += t1[n][f]*L[m][n][e+no][f+no];
      Fov_[m][e] = value;
    }
}

void CCWavefunction::build_W()
{
  int no = no_;
  int nv = nv_;
  double ****tau = tau_;
  double **t1 = t1old_;
  double ****t2 = t2old_;
  double ****ints = H_->ints_;
  double ****L = H_->L_;

  for(int m=0; m < no; m++)
    for(int n=0; n < no; n++)
      for(int i=0; i < no; i++)
        for(int j=0; j < no; j++) {
          double value = ints[m][n][i][j];
          for(int e=0; e < nv; e++) {
            value += t1[j][e]*ints[m][n][i][e+no] +
                     t1[i][e]*ints[m][n][e+no][j];
            for(int f=0; f < nv; f++)
              value += tau[i][j][e][f]*ints[m][n][e+no][f+no];
          }
          Woooo_[m][n][i][j] = value;
        }

  for(int m=0; m < no; m++)
    for(int b=0; b < nv; b++)
      for(int j=0; j < no; j++)
        for(int e=0; e < nv; e++) {
          double value = -ints[m][b+no][j][e+no];
          for(int f=0; f < nv; f++)
            value -= t1[j][f]*ints[m][b+no][f+no][e+no];
          for(int n=0; n < no; n++)
            value += t1[n][b]*ints[m][n][j][e+no];
          for(int n=0; n < no; n++) {
            for(int f=0; f < nv; f++)
              value += ints[m][n][f+no][e+no]*
                (0.5*t2[j][n][f][b] + t1[j][f]*t1[n][b]);
          }
          Wovov_[m][b][j][e] = value;
        }

  for(int m=0; m < no; m++)
    for(int b=0; b < nv; b++)
      for(int e=0; e < nv; e++)
        for(int j=0; j < no; j++) {
          double value = ints[m][b+no][e+no][j];
          for(int f=0; f < nv; f++)
            value += t1[j][f]*ints[m][b+no][e+no][f+no];
          for(int n=0; n < no; n++)
            value -= t1[n][b]*ints[m][n][e+no][j];
          for(int n=0; n < no; n++)
            for(int f=0; f < nv; f++)
              value -= ints[m][n][e+no][f+no]*
                (0.5*t2[j][n][f][b] + t1[j][f]*t1[n][b]);
          for(int n=0; n < no; n++) {
            for(int f=0; f < nv; f++)
              value += 0.5*L[m][n][e+no][f+no]*t2[n][j][f][b];
          }
          Wovvo_[m][b][e][j] = value;
        }
}

void CCWavefunction::build_t1()
{
  int no = no_;
  int nv = nv_;
  double **t1new = t1_;
  double **t1 = t1old_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;
  double ****L = H_->L_;
  double **Fae = Fvv_;
  double **Fmi = Foo_;
  double **Fme = Fov_;
  double ****t2 = t2old_;

  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      double value = fock[a+no][i];
      for(int e=0; e < nv; e++)
        value += t1[i][e]*Fae[a][e];
      for(int m=0; m < no; m++)
        value -= t1[m][a]*Fmi[m][i];
      for(int m=0; m < no; m++)
        for(int e=0; e < nv; e++)
          value += (2*t2[i][m][a][e]-t2[i][m][e][a])*Fme[m][e];
      for(int n=0; n < no; n++)
        for(int f=0; f < nv; f++)
          value += t1[n][f]*L[n][a+no][f+no][i];
      for(int m=0; m < no; m++)
        for(int e=0; e < nv; e++)
          for(int f=0; f < nv; f++)
            value += (2*t2[m][i][e][f]-t2[m][i][f][e])*ints[m][a+no][e+no][f+no];
      for(int m=0; m < no; m++)
        for(int e=0; e < nv; e++)
          for(int n=0; n < no; n++)
            value -= t2[m][n][a][e]*L[n][m][e+no][i];
      t1new[i][a] = value;
   }
}

void CCWavefunction::build_t2()
{
  int no = no_;
  int nv = nv_;
  double ****t2new = t2_;
  double ****t2 = t2old_;
  double ****tau = tau_;
  double ****Wmnij = Woooo_;
  double ****Wmbej = Wovvo_;
  double ****Wmbje = Wovov_;
  double **t1 = t1old_;
  double **Fme = Fov_;
  double **Fmi = Foo_;
  double **Fae = Fvv_;
  double ****ints = H_->ints_;

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          double value = ints[i][j][a+no][b+no];
          for(int e=0; e < nv; e++)
            value += t2[i][j][a][e]*Fae[b][e] + t2[j][i][b][e]*Fae[a][e];
          for(int e=0; e < nv; e++)
            for(int m=0; m < no; m++)
              value -= 0.5*(t2[i][j][a][e]*t1[m][b]*Fme[m][e] +
                      t2[i][j][e][b]*t1[m][a]*Fme[m][e]);
          for(int m=0; m < no; m++)
            value -=  t2[i][m][a][b]*Fmi[m][j] + t2[m][j][a][b]*Fmi[m][i];
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++)
              value -= 0.5*(t2[i][m][a][b]*t1[j][e]*Fme[m][e] +
                      t2[m][j][a][b]*t1[i][e]*Fme[m][e]);
          for(int m=0; m < no; m++)
            for(int n=0; n < no; n++)
              value += tau[m][n][a][b]*Wmnij[m][n][i][j];
          for(int e=0; e < nv; e++)
            for(int f=0; f < nv; f++)
              value += tau[i][j][e][f]*ints[a+no][b+no][e+no][f+no];
          for(int e=0; e < nv; e++)
            value += t1[i][e]*ints[a+no][b+no][e+no][j] +
                     t1[j][e]*ints[b+no][a+no][e+no][i];
          for(int m=0; m < no; m++)
            value -= t1[m][a]*ints[m][b+no][i][j]+t1[m][b]*ints[m][a+no][j][i];
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++) {
              value += (t2[i][m][a][e] - t2[i][m][e][a]) * Wmbej[m][b][e][j];
              value += t2[i][m][a][e] * (Wmbej[m][b][e][j] + Wmbje[m][b][j][e]);
              value += t2[m][j][a][e] * Wmbje[m][b][i][e];
              value += t2[i][m][e][b] * Wmbje[m][a][j][e];
              value += t2[j][m][b][e] * (Wmbej[m][a][e][i] + Wmbje[m][a][i][e]);
              value += (t2[j][m][b][e] - t2[j][m][e][b]) * Wmbej[m][a][e][i];
            }
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++) {
              value -= t1[i][e]*t1[m][a]*ints[m][b+no][e+no][j];
              value -= t1[i][e]*t1[m][b]*ints[m][a+no][j][e+no];
              value -= t1[j][e]*t1[m][a]*ints[m][b+no][i][e+no];
              value -= t1[j][e]*t1[m][b]*ints[m][a+no][e+no][i];
            }
          t2new[i][j][a][b] = value;
        }

  double ****Zmbij = init_4d_array(no, nv, no, no);
  for(int m=0; m < no; m++)
    for(int b=0; b < nv; b++)
      for(int i=0; i < no; i++)
        for(int j=0; j < no; j++) {
          Zmbij[m][b][i][j] = 0.0;
          for(int e=0; e < nv; e++)
            for(int f=0; f < nv; f++)
              Zmbij[m][b][i][j] += ints[m][b+no][e+no][f+no] * tau[i][j][e][f];
        }

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          double value = 0.0;
          for(int m=0; m < no; m++)
            value -= t1[m][a]*Zmbij[m][b][i][j];
          t2new[i][j][a][b] += value;
          t2new[j][i][b][a] += value;
        }
  free_4d_array(Zmbij,no,nv,no);
}

double CCWavefunction::t1norm()
{
  int no = no_;
  int nv = nv_;
  double **t1 = t1_;

  double diag = 0.0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      diag += t1[i][a] * t1[i][a];

  return sqrt(diag/(2*no));
}

double CCWavefunction::increment_amps()
{
  int no = no_;
  int nv = nv_;
  double **D1 = D1_;
  double ****D2 = D2_;
  double **t1, **t1old, ****t2, ****t2old;

  t1 = t1_;
  t1old = t1old_;
  t2 = t2_;
  t2old = t2old_;

  double residual1 = 0.0;
  double residual2 = 0.0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      residual1 += t1[i][a] * t1[i][a];     
      t1[i][a] = t1old[i][a] + t1[i][a]/D1[i][a];
      for(int j=0; j < no; j++)
        for(int b=0; b < nv; b++) {
          residual2 += t2[i][j][a][b] * t2[i][j][a][b];
          t2[i][j][a][b] = t2old[i][j][a][b] + t2[i][j][a][b]/D2[i][j][a][b];
        }
    }

  return sqrt(residual1 + residual2);
}

void CCWavefunction::build_tstar()
{
  int no = no_; 
  int nv = nv_;
  double **t1 = t1_;
  double ****t2 = t2_;

  t1s_ = block_matrix(no,nv);
  t2s_ = init_4d_array(no,no,nv,nv);

  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
          t1s_[i][a] = 2.0 * t1[i][a];
      for(int j=0; j < no; j++)
        for(int b=0; b < nv; b++)
          t2s_[i][j][a][b] = 4.0 * t2[i][j][a][b] - 2.0 * t2[i][j][b][a];
    }
}

double CCWavefunction::tcorr()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;
  double ****L = H_->L_;
  double **t1 = t1_;
  double ****t2 = t2_;
  double ******t3;

  // Scandinavian expression for (T) correction
  t3 = init_6d_array(no, no, no, nv, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {

        for(int a=0; a < nv; a++)
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) {
              double value = 0.0;
              for(int e=0; e < nv; e++) {
                value +=
                   + ints[i][e+no][a+no][b+no] * t2[k][j][c][e]
                   + ints[i][e+no][a+no][c+no] * t2[j][k][b][e]
                   + ints[k][e+no][c+no][a+no] * t2[j][i][b][e]
                   + ints[k][e+no][c+no][b+no] * t2[i][j][a][e]
                   + ints[j][e+no][b+no][c+no] * t2[i][k][a][e]
                   + ints[j][e+no][b+no][a+no] * t2[k][i][c][e];
              }
              for(int m=0; m < no; m++) {
                value -=
                   + ints[j][k][m][c+no] * t2[i][m][a][b]
                   + ints[k][j][m][b+no] * t2[i][m][a][c]
                   + ints[i][j][m][b+no] * t2[k][m][c][a]
                   + ints[j][i][m][a+no] * t2[k][m][c][b]
                   + ints[k][i][m][a+no] * t2[j][m][b][c]
                   + ints[i][k][m][c+no] * t2[j][m][b][a];
              }
              double denom = fock[i][i] + fock[j][j] + fock[k][k];
              denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

              t3[i][j][k][a][b][c] = value/denom;
            }
      }
  t3_ = t3;

  double **X1 = block_matrix(no, nv);
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      for(int k=0; k < no; k++)
        for(int l=0; l < no; l++)
          for(int c=0; c < nv; c++)
            for(int d=0; d < nv; d++)
              X1[i][a] += (t3[i][k][l][a][c][d] - t3[l][k][i][a][c][d]) * L[k][l][c+no][d+no];

  double ****X2 = init_4d_array(no, no, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              X2[i][j][a][b] += (t3[i][j][k][a][b][c] - t3[k][j][i][a][b][c]) * fock[k][c+no];

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              for(int l=0; l < no; l++) {
                X2[i][j][a][b] -= t3[i][k][l][a][b][c] * L[k][l][j][c+no];
                X2[i][j][a][b] += t3[l][k][i][a][b][c] * ints[k][l][j][c+no];
              }

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              for(int d=0; d < nv; d++) {
                X2[i][j][a][b] += t3[i][j][k][a][c][d] * L[b+no][k][c+no][d+no];
                X2[i][j][a][b] -= t3[k][j][i][a][c][d] * ints[b+no][k][c+no][d+no];
              }
        }
  

  double ET_UGA = 0.0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      ET_UGA += t1[i][a] * X1[i][a];
      for(int j=0; j < no; j++)
        for(int b=0; b < nv; b++)
          ET_UGA += (2.0*t2[i][j][a][b] - t2[i][j][b][a]) * X2[i][j][a][b];    
    }
  ET_UGA *= 2.0;

  free_block(X1);
  free_4d_array(X2, no, no, nv);

  return ET_UGA;
}

void CCWavefunction::t3_ijk(double ***t3, int i, int j, int k, double ****t2, double **fock, double ****ints)
{
  int no = no_;
  int nv = nv_;

  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++)
      for(int c=0; c < nv; c++) {
        double value = 0.0;
        for(int e=0; e < nv; e++) {
          value +=
            + ints[i][e+no][a+no][b+no] * t2[k][j][c][e]
            + ints[i][e+no][a+no][c+no] * t2[j][k][b][e]
            + ints[k][e+no][c+no][a+no] * t2[j][i][b][e]
            + ints[k][e+no][c+no][b+no] * t2[i][j][a][e]
            + ints[j][e+no][b+no][c+no] * t2[i][k][a][e]
            + ints[j][e+no][b+no][a+no] * t2[k][i][c][e];
        }
        for(int m=0; m < no; m++) {
          value -=
            + ints[j][k][m][c+no] * t2[i][m][a][b]
            + ints[k][j][m][b+no] * t2[i][m][a][c]
            + ints[i][j][m][b+no] * t2[k][m][c][a]
            + ints[j][i][m][a+no] * t2[k][m][c][b]
            + ints[k][i][m][a+no] * t2[j][m][b][c]
            + ints[i][k][m][c+no] * t2[j][m][b][a];
        }
        double denom = fock[i][i] + fock[j][j] + fock[k][k];
        denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

        t3[a][b][c] = value/denom;
      } // abc

  return;
}

void CCWavefunction::t3_abc(double ***t3, int a, int b, int c, double ****t2, double **fock, double ****ints)
{
  int no = no_;
  int nv = nv_;

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {
        double value = 0.0;
        for(int e=0; e < nv; e++) {
          value +=
            + ints[i][e+no][a+no][b+no] * t2[k][j][c][e]
            + ints[i][e+no][a+no][c+no] * t2[j][k][b][e]
            + ints[k][e+no][c+no][a+no] * t2[j][i][b][e]
            + ints[k][e+no][c+no][b+no] * t2[i][j][a][e]
            + ints[j][e+no][b+no][c+no] * t2[i][k][a][e]
            + ints[j][e+no][b+no][a+no] * t2[k][i][c][e];
        }     
        for(int m=0; m < no; m++) {
          value -=
            + ints[j][k][m][c+no] * t2[i][m][a][b]
            + ints[k][j][m][b+no] * t2[i][m][a][c]
            + ints[i][j][m][b+no] * t2[k][m][c][a]
            + ints[j][i][m][a+no] * t2[k][m][c][b]
            + ints[k][i][m][a+no] * t2[j][m][b][c]
            + ints[i][k][m][c+no] * t2[j][m][b][a];
        }
        double denom = fock[i][i] + fock[j][j] + fock[k][k];
        denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

        t3[i][j][k] = value/denom;
      } // ijk
}

double CCWavefunction::tcorr_ooc()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;
  double ****L = H_->L_;
  double ****t2 = t2_;
  double **t1s = t1s_;
  double ****t2s = t2s_;

  double ***t3 = init_3d_array(nv, nv, nv);
  double **X1 = block_matrix(no, nv);
  double ****X2 = init_4d_array(no, no, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {
        t3_ijk(t3, i, j, k, t2, fock, ints);

        for(int a=0; a < nv; a++) 
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) {
              X1[i][a] += (t3[a][b][c] - t3[c][b][a]) * L[j][k][b+no][c+no];

              X2[i][j][a][b] += (t3[a][b][c] - t3[c][b][a]) * fock[k][c+no];

              for(int l=0; l < no; l++)
                X2[i][l][a][b] -= (2.0*t3[a][b][c] - t3[a][c][b] - t3[c][b][a]) * ints[j][k][l][c+no];

              for(int d=0; d < nv; d++)
                X2[i][j][a][d] += (2.0*t3[a][b][c] - t3[a][c][b] - t3[c][b][a]) * ints[d+no][k][b+no][c+no];
            } // abc
      } // ijk

  double ET_UGA = 0.0;
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      ET_UGA += t1s[i][a] * X1[i][a];
      for(int j=0; j < no; j++)
        for(int b=0; b < nv; b++)
          ET_UGA += t2s[i][j][a][b] * X2[i][j][a][b];
    }

  free_block(X1);
  free_4d_array(X2, no, no, nv);
  free_3d_array(t3, nv, nv);

  return ET_UGA;
}

void CCWavefunction::W3_ijk(double ***W3, int i, int j, int k, double ****t2, double ****ints)
{
  int no = no_;
  int nv = nv_;

  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++)
      for(int c=0; c < nv; c++) {
        double value = 0.0;
        for(int e=0; e < nv; e++) {
          value +=
            + ints[i][e+no][a+no][b+no] * t2[k][j][c][e]
            + ints[i][e+no][a+no][c+no] * t2[j][k][b][e]
            + ints[k][e+no][c+no][a+no] * t2[j][i][b][e]
            + ints[k][e+no][c+no][b+no] * t2[i][j][a][e]
            + ints[j][e+no][b+no][c+no] * t2[i][k][a][e]
            + ints[j][e+no][b+no][a+no] * t2[k][i][c][e];
        }
        for(int m=0; m < no; m++) {
          value -=
            + ints[j][k][m][c+no] * t2[i][m][a][b]
            + ints[k][j][m][b+no] * t2[i][m][a][c]
            + ints[i][j][m][b+no] * t2[k][m][c][a]
            + ints[j][i][m][a+no] * t2[k][m][c][b]
            + ints[k][i][m][a+no] * t2[j][m][b][c]
            + ints[i][k][m][c+no] * t2[j][m][b][a];
        }

        W3[a][b][c] = value;
      } // abc
}

double CCWavefunction::tcorr_ooc_TJL()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;
  double **t1 = t1_;
  double ****t2 = t2_;

  double ***W3 = init_3d_array(nv, nv, nv);
  double ***V3 = init_3d_array(nv, nv, nv);
  double ***X3 = init_3d_array(nv, nv, nv);
  double ***Y3 = init_3d_array(nv, nv, nv);
  double ***Z3 = init_3d_array(nv, nv, nv);
  double ET = 0.0;
  for(int i=0; i < no; i++)
    for(int j=0; j <= i; j++)
      for(int k=0; k <= j; k++) {
        W3_ijk(W3, i, j, k, t2, ints);

        for(int a=0; a < nv; a++)
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++)
              V3[a][b][c] = (W3[a][b][c] + ints[i][j][a+no][b+no] * t1[k][c]
                          + ints[i][k][a+no][c+no] * t1[j][b]
                          + ints[j][k][b+no][c+no] * t1[i][a])/(1.0+(a==b)+(a==c)+(b==c));

        for(int a=0; a < nv; a++)
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) {
              X3[a][b][c] = W3[a][b][c] * V3[a][b][c] + W3[a][c][b] * V3[a][c][b] 
                          + W3[b][a][c] * V3[b][a][c] + W3[b][c][a] * V3[b][c][a]
                          + W3[c][a][b] * V3[c][a][b] + W3[c][b][a] * V3[c][b][a];
              Y3[a][b][c] = V3[a][b][c] + V3[b][c][a] + V3[c][a][b];
              Z3[a][b][c] = V3[a][c][b] + V3[b][a][c] + V3[c][b][a];
            }

        for(int a=0; a < nv; a++)
          for(int b=0; b <= a; b++)
            for(int c=0; c <= b; c++) {
              double denom = fock[i][i] + fock[j][j] + fock[k][k];
              denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

              ET += ((Y3[a][b][c] - 2.0 * Z3[a][b][c]) * (W3[a][b][c] + W3[b][c][a] + W3[c][a][b])
                  + (Z3[a][b][c] - 2.0 * Y3[a][b][c]) * (W3[a][c][b] + W3[b][a][c] + W3[c][b][a])
                  + 3.0 * X3[a][b][c]) * (2.0 - ((i==j) + (i==k) + (j==k)))/denom;
            }

      } // ijk

  free_3d_array(W3, nv, nv);
  free_3d_array(V3, nv, nv);
  free_3d_array(X3, nv, nv);
  free_3d_array(Y3, nv, nv);
  free_3d_array(Z3, nv, nv);

  return ET;
}

void CCWavefunction::tgrad()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;
  double ****L = H_->L_;
  double **t1 = t1_;
  double ****t2 = t2_;
  double ******t3 = t3_;
  double **Doo = Doo_;
  double **Dvv = Dvv_;
  double ****Gooov = Gooov_;
  double ****Gvvvo = Gvvvo_;
  double ****Goovv = Goovv_;

  double **s1;
  double ****s2;

  // T3 --> Lambda1
  s1 = block_matrix(no, nv);
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++)
      for(int k=0; k < no; k++)
        for(int l=0; l < no; l++)
          for(int c=0; c < nv; c++)
            for(int d=0; d < nv; d++)
              s1[i][a] += 2.0 * (t3[i][k][l][a][c][d] - t3[k][i][l][a][c][d]) * L[k][l][c+no][d+no];

  s1_ = s1;

  // T3 --> Lambda2
  double ****Y2 = init_4d_array(no, no, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          double value = 0.0;         

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              value += (t3[i][j][k][a][b][c] - t3[k][j][i][a][b][c]) * fock[k][c+no];

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              for(int l=0; l < no; l++)
                value -= (2.0 * t3[j][k][l][b][a][c] - t3[j][l][k][b][a][c] - t3[l][k][j][b][a][c]) * ints[k][l][i][c+no];

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              for(int d=0; d < nv; d++)
                value += (2.0 * t3[j][i][k][b][c][d] - t3[j][k][i][b][c][d] - t3[k][i][j][b][c][d]) * ints[a+no][k][c+no][d+no];

          Y2[i][j][a][b] = value;
        }

  double ****X2 = init_4d_array(no, no, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
           X2[i][j][a][b] += Y2[i][j][a][b] + Y2[j][i][b][a];

  s2 = init_4d_array(no, no, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          s2[i][j][a][b] += 4.0 * X2[i][j][a][b] - 2.0 * X2[i][j][b][a];
  s2_ = s2;

  // Lambda3 amplitudes
  double **t1s = t1s_;
  double ****t2s = t2s_;
  double ******l3_tmp = init_6d_array(no, no, no, nv, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {

        for(int a=0; a < nv; a++)
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) {
              double value = 0.0;
  
              value += fock[i][a+no] * t2s[j][k][b][c] - fock[j][a+no] * t2s[i][k][b][c];
              value += L[i][j][a+no][b+no] * t1s[k][c] - L[i][j][a+no][c+no] * t1s[k][b];

              for(int f=0; f < nv; f++) {
                value += L[f+no][j][a+no][b+no] * t2s[i][k][f][c];
                value -= ints[k][f+no][a+no][b+no] * t2s[i][j][c][f];
              }
              for(int n=0; n < no; n++) {
                value -= L[i][j][a+no][n] * t2s[n][k][b][c];
                value += ints[k][j][a+no][n] * t2s[n][i][b][c];
              }

              double denom = fock[i][i] + fock[j][j] + fock[k][k];
              denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

              l3_tmp[i][j][k][a][b][c] = value/denom;
            }
      }

  double ******l3 = init_6d_array(no, no, no, nv, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++)
        for(int a=0; a < nv; a++)
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) 
              l3[i][j][k][a][b][c] = l3_tmp[i][j][k][a][b][c]
                                   + l3_tmp[i][k][j][a][c][b]
                                   + l3_tmp[j][i][k][b][a][c]
                                   + l3_tmp[j][k][i][b][c][a]
                                   + l3_tmp[k][i][j][c][a][b]
                                   + l3_tmp[k][j][i][c][b][a];

  l3_ = l3;
  free_6d_array(l3_tmp, no, no, no, nv, nv);

  // Lambda3 --> Lambda2
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          Y2[i][j][a][b] = 0.0;

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              for(int l=0; l < no; l++)
                Y2[i][j][a][b] -= l3[i][k][l][a][c][b] * ints[c+no][j][k][l];

          for(int k=0; k < no; k++)
            for(int c=0; c < nv; c++)
              for(int d=0; d < nv; d++)
                Y2[i][j][a][b] += l3[i][k][j][a][c][d] * ints[c+no][d+no][k][b+no];
        }

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++)
          s2[i][j][a][b] += Y2[i][j][a][b] + Y2[j][i][b][a];

  free_4d_array(X2, no, no, nv);
  free_4d_array(Y2, no, no, nv);

  // (T) density contributions
  // OO
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++) {
      Doo[i][j] = 0.0;
      for(int l=0; l < no; l++)
        for(int m=0; m < no; m++) 
          for(int d=0; d < nv; d++)
            for(int e=0; e < nv; e++)
              for(int f=0; f < nv; f++)
                Doo[i][j] -= 0.5 * t3[i][l][m][d][e][f] * l3[j][l][m][d][e][f];
    }

  // VV
  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++) {
      Dvv[a][b] = 0.0;
      for(int l=0; l < no; l++)
        for(int m=0; m < no; m++)
          for(int n=0; n < no; n++)
            for(int d=0; d < nv; d++)
              for(int e=0; e < nv; e++)
                Dvv[a][b] += 0.5 * t3[l][m][n][b][d][e] * l3[l][m][n][a][d][e];
    }

  // OV
/*
  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) {
      Dov[i][a] = 0.0;
      for(int m=0; m < no; m++)
        for(int n=0; n < no; n++)
          for(int e=0; e < nv; e++)
            for(int f=0; f < nv; f++)
              Dov[i][a] += (t3[m][n][i][e][f][a] - t3[m][i][n][e][f][a]) * t2s[m][n][e][f];
    }
*/

  // OOOV
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++)
        for(int a=0; a < nv; a++) {
          Gooov[i][j][k][a] = 0.0;
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++)
              for(int f=0; f < nv; f++)
                Gooov[i][j][k][a] -= (4.0 * t2[k][m][e][f] - 2.0 * t2[k][m][f][e]) *
                        (2.0 * t3[j][i][m][a][e][f] - t3[i][j][m][a][e][f] - t3[m][i][j][a][e][f]);

          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++)
              for(int f=0; f < nv; f++)
                Gooov[i][j][k][a] -= t2[k][m][e][f] * l3[m][j][i][f][a][e];
        }

  // VVVO
  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++)
      for(int c=0; c < nv; c++)
        for(int i=0; i < no; i++) {
          Gvvvo[a][b][c][i] = 0.0;
          for(int m=0; m < no; m++)
            for(int n=0; n < no; n++)
              for(int e=0; e < nv; e++)
                Gvvvo[a][b][c][i] += (4.0 * t2[m][n][e][c] - 2.0 * t2[m][n][c][e]) *
                      (2.0 * t3[n][i][m][a][b][e] - t3[n][i][m][b][a][e] - t3[n][i][m][a][e][b]);

          for(int m=0; m < no; m++)
            for(int n=0; n < no; n++)
              for(int e=0; e < nv; e++)
                Gvvvo[a][b][c][i] += t2[m][n][e][c] * l3[n][i][m][a][b][e];
        }

  // OOVV
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int a=0; a < nv; a++)
        for(int b=0; b < nv; b++) {
          Goovv[i][j][a][b] = 0.0;
          for(int m=0; m < no; m++)
            for(int e=0; e < nv; e++)
              Goovv[i][j][a][b] += 4.0 * t1[m][e] *
                  ( 2.0 * (t3[i][j][m][a][b][e] - t3[i][j][m][a][e][b]) - (t3[i][j][m][b][a][e] - t3[i][j][m][b][e][a]) );
        }

  return;
}

void CCWavefunction::M3_ijk(double ***M3, int i, int j, int k, double ****t2, double **fock, double ****ints)
{
  int no = no_;
  int nv = nv_;

  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++)
      for(int c=0; c < nv; c++) {
        double value = 0.0;
        for(int e=0; e < nv; e++) {
          value +=
            + ints[i][e+no][a+no][b+no] * t2[k][j][c][e]
            + ints[i][e+no][a+no][c+no] * t2[j][k][b][e]
            + ints[k][e+no][c+no][a+no] * t2[j][i][b][e]
            + ints[k][e+no][c+no][b+no] * t2[i][j][a][e]
            + ints[j][e+no][b+no][c+no] * t2[i][k][a][e]
            + ints[j][e+no][b+no][a+no] * t2[k][i][c][e];
        }
        for(int m=0; m < no; m++) {
          value -=
            + ints[j][k][m][c+no] * t2[i][m][a][b]
            + ints[k][j][m][b+no] * t2[i][m][a][c]
            + ints[i][j][m][b+no] * t2[k][m][c][a]
            + ints[j][i][m][a+no] * t2[k][m][c][b]
            + ints[k][i][m][a+no] * t2[j][m][b][c]
            + ints[i][k][m][c+no] * t2[j][m][b][a];
        }
        double denom = fock[i][i] + fock[j][j] + fock[k][k];
        denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

        M3[a][b][c] = value/denom;
      } // abc

  return;
}

void CCWavefunction::M3_abc(double ***M3, int a, int b, int c, double ****t2, double **fock, double ****ints)
{
  int no = no_;
  int nv = nv_;

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {
        double value = 0.0;
        for(int e=0; e < nv; e++) {
          value +=
            + ints[i][e+no][a+no][b+no] * t2[k][j][c][e]
            + ints[i][e+no][a+no][c+no] * t2[j][k][b][e]
            + ints[k][e+no][c+no][a+no] * t2[j][i][b][e]
            + ints[k][e+no][c+no][b+no] * t2[i][j][a][e]
            + ints[j][e+no][b+no][c+no] * t2[i][k][a][e]
            + ints[j][e+no][b+no][a+no] * t2[k][i][c][e];
        }
        for(int m=0; m < no; m++) {
          value -=
            + ints[j][k][m][c+no] * t2[i][m][a][b]
            + ints[k][j][m][b+no] * t2[i][m][a][c]
            + ints[i][j][m][b+no] * t2[k][m][c][a]
            + ints[j][i][m][a+no] * t2[k][m][c][b]
            + ints[k][i][m][a+no] * t2[j][m][b][c]
            + ints[i][k][m][c+no] * t2[j][m][b][a];
        }
        double denom = fock[i][i] + fock[j][j] + fock[k][k];
        denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

        M3[i][j][k] = value/denom;
      } // ijk

  return;
}

void CCWavefunction::N3_ijk(double ***N3, int i, int j, int k, double ****t2, double **t1, double **fock, double ****ints)
{
  int no = no_;
  int nv = nv_;

  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++)
      for(int c=0; c < nv; c++) {
        double value = 0.0;

        value = ints[i][j][a+no][b+no] * t1[k][c]
              + ints[i][k][a+no][c+no] * t1[j][b]
              + ints[j][k][b+no][c+no] * t1[i][a]
              + t2[i][j][a][b] * fock[k][c+no]
              + t2[i][k][a][c] * fock[j][b+no]
              + t2[j][k][b][c] * fock[i][a+no];

        double denom = fock[i][i] + fock[j][j] + fock[k][k];
        denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

        N3[a][b][c] = value/denom;
      } // abc

}

void CCWavefunction::N3_abc(double ***N3, int a, int b, int c, double ****t2, double **t1, double **fock, double ****ints)
{
  int no = no_;

  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {
        double value = 0.0;

        value = ints[i][j][a+no][b+no] * t1[k][c]
              + ints[i][k][a+no][c+no] * t1[j][b]
              + ints[j][k][b+no][c+no] * t1[i][a]
              + t2[i][j][a][b] * fock[k][c+no]
              + t2[i][k][a][c] * fock[j][b+no]
              + t2[j][k][b][c] * fock[i][a+no];

        double denom = fock[i][i] + fock[j][j] + fock[k][k];
        denom -= fock[a+no][a+no] + fock[b+no][b+no] + fock[c+no][c+no];

        N3[i][j][k] = value/denom;
      } // ijk
}

/*
  tgrad_ooc(): Computes triples contributes to the CCSD(T) gradient using a
  triples-driven algorithm.  Both T3 and L3 amplitudes are computed in VVV
  batches for a given combination of OOO indices.

  -TDC, 1/2014
*/
void CCWavefunction::tgrad_ooc()
{
  int no = no_;
  int nv = nv_;
  double **fock = H_->fock_;
  double ****ints = H_->ints_;
  double **t1 = t1_;
  double ****t2 = t2_;

  double **s1 = block_matrix(no, nv);
  double ****s2 = init_4d_array(no, no, nv, nv);
  s1_ = s1;
  s2_ = s2;
  double **Doo = Doo_;
  double **Dvv = Dvv_;
  double ****Goovv = Goovv_;
  double ****Gooov = Gooov_;
  double ****Gvvvo = Gvvvo_;

  double **X1 = block_matrix(no, nv); // T3 --> L1
  double ****X2 = init_4d_array(no, no, nv, nv); // T3 & L3 --> L2

  double ***M3 = init_3d_array(nv, nv, nv);
  double ***N3 = init_3d_array(nv, nv, nv);
  double ***X3 = init_3d_array(nv, nv, nv);
  double ***Y3 = init_3d_array(nv, nv, nv);
  for(int i=0; i < no; i++)
    for(int j=0; j < no; j++)
      for(int k=0; k < no; k++) {
        M3_ijk(M3, i, j, k, t2, fock, ints);
        N3_ijk(N3, i, j, k, t2, t1, fock, ints);

        for(int a=0; a < nv; a++) 
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) {
              X3[a][b][c] = 8.0*M3[a][b][c]-4.0*M3[b][a][c]-4.0*M3[a][c][b]-4.0*M3[c][b][a]+2.0*M3[c][a][b]+2.0*M3[b][c][a];
              Y3[a][b][c] = 8.0*N3[a][b][c]-4.0*N3[b][a][c]-4.0*N3[a][c][b]-4.0*N3[c][b][a]+2.0*N3[c][a][b]+2.0*N3[b][c][a];
            }

        for(int a=0; a < nv; a++) 
          for(int b=0; b < nv; b++)
            for(int c=0; c < nv; c++) {

              Dvv[a][a] += 0.5 * M3[a][b][c] * (X3[a][b][c] + Y3[a][b][c]);
              s1[i][a] += (4.0*M3[a][b][c] - 2.0*M3[c][b][a] - 2.0*M3[a][c][b] + M3[b][c][a]) * ints[j][k][b+no][c+no];
              Goovv[i][j][a][b] += 4.0 * t1[k][c] * (2.0*(M3[a][b][c] - M3[a][c][b]) - (M3[b][a][c] - M3[b][c][a]));

              for(int l=0; l < no; l++) {
                X2[i][l][a][b] -= (2.0 * X3[a][b][c] + Y3[a][b][b]) * ints[j][k][l][c+no];
                Gooov[j][i][l][a] -= (2.0 * X3[a][b][c] + Y3[a][b][c]) * t2[l][k][b][c];
              }

              for(int d=0; d < nv; d++) {
                X2[i][j][a][d] += (2.0 * X3[a][b][c] + Y3[a][b][c]) * ints[d+no][k][b+no][c+no];
                Gvvvo[a][b][d][j] += (2.0 * X3[a][b][c] + Y3[a][b][c]) * t2[k][i][c][d];
              }

            } // abc

      } // ijk
  free_3d_array(M3, nv, nv);
  free_3d_array(N3, nv, nv);
  free_3d_array(X3, nv, nv);
  free_3d_array(Y3, nv, nv);

  M3 = init_3d_array(no, no, no);
  N3 = init_3d_array(no, no, no);
  X3 = init_3d_array(no, no, no);
  Y3 = init_3d_array(no, no, no);
  for(int a=0; a < nv; a++)
    for(int b=0; b < nv; b++)
      for(int c=0; c < nv; c++) {
        M3_abc(M3, a, b, c, t2, fock, ints);
        N3_abc(N3, a, b, c, t2, t1, fock, ints);

        for(int i=0; i < no; i++)
          for(int j=0; j < no; j++)
            for(int k=0; k < no; k++) {
              X3[i][j][k] = 8.0*M3[i][j][k]-4.0*M3[j][i][k]-4.0*M3[i][k][j]-4.0*M3[k][j][i]+2.0*M3[k][i][j]+2.0*M3[j][k][i];
              Y3[i][j][k] = 8.0*N3[i][j][k]-4.0*N3[j][i][k]-4.0*N3[i][k][j]-4.0*N3[k][j][i]+2.0*N3[k][i][j]+2.0*N3[j][k][i];
            }

        for(int i=0; i < no; i++)
          for(int j=0; j < no; j++)
            for(int k=0; k < no; k++)
                Doo[i][i] -= 0.5 * M3[i][j][k] * (X3[i][j][k] + Y3[i][j][k]);
           
      } // abc
  free_3d_array(M3, no, no);
  free_3d_array(N3, no, no);
  free_3d_array(X3, no, no);
  free_3d_array(Y3, no, no);

  for(int i=0; i < no; i++)
    for(int a=0; a < nv; a++) 
      for(int j=0; j < no; j++)
        for(int b=0; b < nv; b++) {
          s2[i][j][a][b] = X2[i][j][a][b] + X2[j][i][b][a];
        }

  free_block(X1);
  free_4d_array(X2, no, no, nv);

  return;
}

}} // psi::ugacc