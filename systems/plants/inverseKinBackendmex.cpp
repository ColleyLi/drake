#include <mex.h>
#include <math.h>
#include <float.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include "f2c.h"
#include "snopt.hh"
#include "snfilewrapper.hh"
#include "snoptProblem.hh"
#undef max
#undef min

#include "RigidBodyManipulator.h"
#include "constraint/Constraint.h"
#include "drakeUtil.h"
#include <Eigen/LU>

using namespace Eigen;
using namespace std;

RigidBodyManipulator* model = NULL;
KinematicConstraint** kc_array = NULL;
QuasiStaticConstraint* qsc_ptr = NULL;
MatrixXd q_seed;
MatrixXd q_nom;
VectorXd q_nom_i;
MatrixXd q_sol;
MatrixXd Q;
MatrixXd Qa;
MatrixXd Qv;
VectorXd q0;
VectorXd qdot0;
bool quasiStaticFlag;
double shrinkFactor;
integer nx;
integer nF;
int nC;
integer nG;
integer* nc_array;
integer* nG_array;
integer* nA_array;
int nq;
double *t = NULL;
double* ti = NULL;
int num_kc;
int nT;
int num_qsc_pts;
// The following variables are used in inverseKinSequence only
int* q_idx;
int* qdotf_idx;

MatrixXd velocity_mat;
MatrixXd accel_mat;
MatrixXd accel_mat_qd0;
MatrixXd accel_mat_qdf;

// for debug only
MatrixXd q_mat;
MatrixXd qdot_mat;
MatrixXd qddot_mat;
MatrixXd qdiff_mat;



void IK_constraint_fun(double* x,double* c, double* G)
{
  double* qsc_weights;
  if(quasiStaticFlag)
  {
    qsc_weights = x+nq;
  }
  int nc_accum = 0;
  int ng_accum = 0;
  int nc;
  for(int i = 0;i<num_kc;i++)
  {
    nc = kc_array[i]->getNumConstraint(ti);
    VectorXd cnst(nc);
    MatrixXd dcnst(nc,nq);
    kc_array[i]->eval(ti,cnst,dcnst);
    memcpy(&c[nc_accum],cnst.data(),sizeof(double)*nc);
    memcpy(&G[ng_accum],dcnst.data(),sizeof(double)*nc*nq);
    nc_accum += nc;
    ng_accum += nc*nq;
  }
  if(quasiStaticFlag)
  {
    Vector2d cnst;
    MatrixXd dcnst(2,nq+num_qsc_pts);
    qsc_ptr->eval(qsc_weights,cnst,dcnst);
    memcpy(c+nc_accum,cnst.data(),sizeof(double)*2);
    c[nc_accum+3] = 0.0;
    memcpy(G+ng_accum,dcnst.data(),sizeof(double)*dcnst.size());
    nc_accum += 3;
    ng_accum += dcnst.size();
  }
} 

void IK_cost_fun(double* x, double &J, double* dJ)
{
  VectorXd q(nq);
  memcpy(q.data(),x,sizeof(double)*nq);
  VectorXd q_err = q-q_nom_i;
  J = q_err.transpose()*Q*q_err;
  VectorXd dJ_vec = 2*q_err.transpose()*Q;
  memcpy(dJ, dJ_vec.data(),sizeof(double)*nq);
}

int snoptIKfun(integer *Status, integer *n, doublereal x[],
    integer *needF, integer *neF, doublereal F[],
    integer *needG, integer *neG, doublereal G[],
    char *cu, integer *lencu,
    integer iu[], integer *leniu,
    doublereal ru[], integer *lenru)
{
  double* q = x;
  model->doKinematics(q);
  IK_cost_fun(x,F[0],G);
  IK_constraint_fun(x,&F[1],&G[nq]);
  return 0;
}

void IKseq_cost_fun(double* x,double &J,double* dJ)
{
  VectorXd dJ_vec = VectorXd::Zero(nq*nT);
  VectorXd qdotf(nq);
  for(int i = 0;i<nq;i++)
  {
    qdotf(i) = x[qdotf_idx[i]];
  }
  MatrixXd q(nq*nT,1);
  MatrixXd qdot(nq*(nT-1),1);
  MatrixXd qddot(nq*nT,1);
  memcpy(q.data(),q0.data(),sizeof(double)*nq);
  for(int i = 0;i<nq*(nT-1);i++)
  {
    q(nq+i) = x[q_idx[i]];
  }
  qdot.block(0,0,nq*(nT-2),1) = velocity_mat*q;
  qdot.block(nq*(nT-2),0,nq,1) = qdotf;
  qddot = accel_mat*q+accel_mat_qd0*qdot0+accel_mat_qdf*qdotf;
  q.resize(nq,nT);
  qdot.resize(nq,nT-1);
  qddot.resize(nq,nT);
  MatrixXd q_diff = q.block(0,1,nq,nT-1)-q_nom.block(0,1,nq,nT-1);
  q_mat = q;
  qdot_mat = qdot;
  qddot_mat = qddot;
  qdiff_mat = q_diff;
  J = 0.5*qddot.col(0).transpose()*Qa*qddot.col(0);
  for(int i = 1;i<nT;i++)
  {
    J = J + 0.5*qddot.col(i).transpose()*Qa*qddot.col(i)+0.5*qdot.col(i-1).transpose()*Qv*qdot.col(i-1)+0.5*q_diff.col(i-1).transpose()*Q*q_diff.col(i-1);
  }
  MatrixXd tmp1,tmp2,tmp3,tmp4,tmp5,tmp6,tmp7,tmp8,tmp9;
  tmp1 = Qv*qdot.block(0,0,nq,nT-2);
  tmp1.resize(1,nq*(nT-2));
  tmp2 = velocity_mat.block(0,nq,(nT-2)*nq,nq*(nT-1));
  tmp3 = tmp1*tmp2;
  tmp3.resize(nq*(nT-1),1);
  dJ_vec.head(nq*(nT-1)) = tmp3;
  tmp4 = Q*q_diff;
  tmp4.resize(nq*(nT-1),1);
  dJ_vec.head(nq*(nT-1)) = dJ_vec.head(nq*(nT-1))+tmp4;
  tmp5 = Qa*qddot;
  tmp5.resize(1,nq*nT);
  tmp6 = accel_mat.block(0,nq,nq*nT,nq*(nT-1));
  tmp7 = tmp5*tmp6;
  tmp7.resize(nq*(nT-1),1);
  dJ_vec.head(nq*(nT-1)) = dJ_vec.head(nq*(nT-1))+tmp7; 
  tmp8 = Qa*qddot;
  tmp8.resize(1,nq*nT);
  tmp9 = tmp8*accel_mat_qdf+Qv*qdotf;
  tmp9.resize(nq,1);
  dJ_vec.tail(nq) = tmp9+Qv*qdotf;
  memcpy(dJ,dJ_vec.data(),sizeof(double)*nq*nT);
}

int snoptIKseqfun(integer *Status, integer *n, doublereal x[],
    integer *needF, integer *neF, doublereal F[],
    integer *needG, integer *neG, doublereal G[],
    char *cu, integer *lencu,
    integer iu[], integer *leniu,
    doublereal ru[], integer *lenru)
{
  IKseq_cost_fun(x,F[0],G);
  int nf_cum = 1;
  int nG_cum = nq*nT;
  for(int i = 1;i<nT;i++)
  {
    double* q;
    if(quasiStaticFlag)
    {
      q = x+(i-1)*(nq+num_qsc_pts);
    }
    else
    {
      q = x+(i-1)*nq;
    }
    model->doKinematics(q);
    ti = &t[i];
    IK_constraint_fun(q,F+nf_cum,G+nG_cum);
    nf_cum += nc_array[i];
    nG_cum += nG_array[i];
  }
}


void mexFunction( int nlhs, mxArray *plhs[],int nrhs, const mxArray *prhs[] )
{
  if(nrhs<7)
  {
    mexErrMsgIdAndTxt("Drake:inverseKinBackendmex:NotEnoughInputs", "Usage varargout = inverseKinBackendmex(model_ptr,mode,t,q_seed,q_nom,constraint_ptr1,constraint_ptr2,IKoptions)");
  }
  model = (RigidBodyManipulator*) getDrakeMexPointer(prhs[0]);
  nq = model->num_dof;
  int mode = (int) mxGetScalar(prhs[1]);
  nT = mxGetNumberOfElements(prhs[2]);
  if(nT > 0)
  {
    t = new double[nT];
    memcpy(t,mxGetPr(prhs[2]),sizeof(double)*nT);
  }
  else if(nT == 0)
  {
    nT = 1;
    t = NULL;
  }

  q_seed.resize(nq,nT);
  q_nom.resize(nq,nT);
  assert(mxGetM(prhs[3])==nq &&mxGetN(prhs[3]) == nT);
  assert(mxGetM(prhs[4])==nq &&mxGetN(prhs[4]) == nT);
  memcpy(q_seed.data(),mxGetPr(prhs[3]),sizeof(double)*nq*nT);
  memcpy(q_nom.data(),mxGetPr(prhs[4]),sizeof(double)*nq*nT);
  int num_constraint = nrhs-6;
  num_kc = 0;
  kc_array = new KinematicConstraint*[num_constraint];
  qsc_ptr = NULL;
  MatrixXd joint_limit_min(nq,nT);
  MatrixXd joint_limit_max(nq,nT);
  for(int i = 0;i<nT;i++)
  {
    memcpy(joint_limit_min.data()+i*nq,model->joint_limit_min,sizeof(double)*nq);
    memcpy(joint_limit_max.data()+i*nq,model->joint_limit_max,sizeof(double)*nq);
  }
  for(int i = 0;i<num_constraint;i++)
  {
    Constraint* constraint = (Constraint*) getDrakeMexPointer(prhs[i+5]);
    DrakeConstraintType constraint_type = constraint->getType();
    if(constraint_type == DrakeConstraintType::KinematicConstraintType)
    {
      kc_array[num_kc] = (KinematicConstraint*) constraint;
      num_kc++;
    }
    else if(constraint_type == DrakeConstraintType::QuasiStaticConstraintType)
    {
      qsc_ptr = (QuasiStaticConstraint*) constraint;
    }
    else if(constraint_type == DrakeConstraintType::PostureConstraintType)
    {
      double* joint_min = new double[nq];
      double* joint_max = new double[nq];
      PostureConstraint* pc = (PostureConstraint*) constraint;
      for(int j = 0;j<nT;j++)
      {
        pc->bounds(&t[j],joint_min,joint_max);
        for(int k = 0;k<nq;k++)
        {
          joint_limit_min(k,j) = (joint_limit_min(k,j)>joint_min[k]? joint_limit_min(k,j):joint_min[k]);
          joint_limit_max(k,j) = (joint_limit_max(k,j)<joint_max[k]? joint_limit_max(k,j):joint_max[k]);
          if(joint_limit_min(k,j)>joint_limit_max(k,j))
          {
            mexErrMsgIdAndTxt("Drake:inverseKinBackendmex:BadInputs","Posture constraint has lower bound larger than upper bound");
          }
        }
      }
      delete[] joint_min;
      delete[] joint_max;
    }
  }
  if(qsc_ptr == NULL)
  {
    quasiStaticFlag = false;
    num_qsc_pts = 0;
  }
  else
  {
    quasiStaticFlag = qsc_ptr->isActive(); 
    num_qsc_pts = qsc_ptr->getNumWeights();
  }
  mxArray* pm;
  int ikoptions_idx = nrhs-1;
  pm = mxGetProperty(prhs[ikoptions_idx],0,"Q");
  Q.resize(nq,nq);
  memcpy(Q.data(),mxGetPr(pm),sizeof(double)*nq*nq);
  pm = mxGetProperty(prhs[ikoptions_idx],0,"SNOPT_MajorIterationsLimit");
  integer SNOPT_MajorIterationsLimit = (integer) mxGetScalar(pm);
  pm = mxGetProperty(prhs[ikoptions_idx],0,"SNOPT_IterationsLimit");
  integer SNOPT_IterationsLimit = (integer) mxGetScalar(pm);
  pm = mxGetProperty(prhs[ikoptions_idx],0,"SNOPT_MajorFeasibilityTolerance");
  double SNOPT_MajorFeasibilityTolerance = mxGetScalar(pm);
  pm = mxGetProperty(prhs[ikoptions_idx],0,"SNOPT_MajorOptimalityTolerance");
  double SNOPT_MajorOptimalityTolerance = mxGetScalar(pm);
  pm = mxGetProperty(prhs[ikoptions_idx],0,"SNOPT_SuperbasicsLimit");
  integer SNOPT_SuperbasicsLimit = (integer) mxGetScalar(pm);
  pm = mxGetProperty(prhs[ikoptions_idx],0,"debug_mode");
  bool* debug_mode_ptr = mxGetLogicals(pm);
  bool debug_mode = *debug_mode_ptr;
  integer* INFO;
  double* INFO_tmp;
  mxArray* infeasible_constraint_cell;
  if(mode == 1)
  {
    mwSize ret_dim[1] = {3};
    plhs[0] = mxCreateCellArray(1,ret_dim);
    INFO = new integer[nT]; 
    INFO_tmp = new double[nT];
    mwSize time_dim[1] = {nT};
    infeasible_constraint_cell = mxCreateCellArray(1,time_dim);
    for(int j = 0;j<nT;j++)
    {
      INFO[j] = 0;
    }
  }
  else if(mode == 2)
  {
    mwSize ret_dim[1] = {5};
    plhs[0] = mxCreateCellArray(1,ret_dim);
    INFO = new integer[1];
    INFO_tmp = new double[1];
    INFO[0] = 0;
  }
  mxArray* q_sol_ptr = mxCreateDoubleMatrix(nq,nT,mxREAL);
  mxArray* info_ptr;
  q_sol.resize(nq,nT);
  memcpy(q_sol.data(),mxGetPr(prhs[3]),sizeof(double)*nq*nT);
  VectorXd* iCfun_array = new VectorXd[nT];
  VectorXd* jCvar_array = new VectorXd[nT];
  nc_array = new integer[nT];
  nG_array = new integer[nT];
  nA_array = new integer[nT];
  for(int i = 0;i<nT;i++)
  {
    nc_array[i] = 0;
    nG_array[i] = 0;
    nA_array[i] = 0;
  }
  VectorXd* A_array = new VectorXd[nT];
  VectorXd* iAfun_array = new VectorXd[nT];
  VectorXd* jAvar_array = new VectorXd[nT];
  VectorXd* Cmin_array = new VectorXd[nT];
  VectorXd* Cmax_array = new VectorXd[nT]; 
  vector<string>* Cname_array = new vector<string>[nT];
  for(int i =0;i<nT;i++)
  {
    Cmin_array[i].resize(0);
    Cmax_array[i].resize(0);
    iCfun_array[i].resize(0);
    jCvar_array[i].resize(0);
    A_array[i].resize(0);
    iAfun_array[i].resize(0);
    jAvar_array[i].resize(0);
  }
  for(int i = 0;i<nT;i++)
  {
    for(int j = 0;j<num_kc;j++)
    {
      if(kc_array[j]->isTimeValid(&t[i]))
      {
        int nc = kc_array[j]->getNumConstraint(&t[i]);
        VectorXd lb,ub;
        lb.resize(nc);
        ub.resize(nc);
        kc_array[j]->bounds(&t[i],lb,ub);
        Cmin_array[i].conservativeResize(Cmin_array[i].size()+nc);
        Cmin_array[i].tail(nc) = lb;
        Cmax_array[i].conservativeResize(Cmax_array[i].size()+nc);
        Cmax_array[i].tail(nc) = ub;
        iCfun_array[i].conservativeResize(iCfun_array[i].size()+nc*nq);
        jCvar_array[i].conservativeResize(jCvar_array[i].size()+nc*nq);
        VectorXd iCfun_append(nc);
        VectorXd jCvar_append(nc);
        for(int k = 0;k<nc;k++)
        {
          iCfun_append(k) = nc_array[i]+k+1;
        }
        for(int k = 0;k<nq;k++)
        {
          iCfun_array[i].segment(nG_array[i]+k*nc,nc) = iCfun_append;
          jCvar_append = VectorXd::Constant(nc,k+1);
          jCvar_array[i].segment(nG_array[i]+k*nc,nc) = jCvar_append;
        }
        nc_array[i] = nc_array[i]+nc;
        nG_array[i] = nG_array[i]+nq*nc;
        if(debug_mode)
        {
          vector<string> constraint_name;
          kc_array[j]->name(&t[i],constraint_name);
          for(int l = 0;l<nc;l++)
          {
            Cname_array[i].push_back(constraint_name[l]);
          }
        }
      }
    }
    if(quasiStaticFlag)
    {
      iCfun_array[i].conservativeResize(iCfun_array[i].size()+2*(nq+num_qsc_pts));
      jCvar_array[i].conservativeResize(jCvar_array[i].size()+2*(nq+num_qsc_pts));
      iAfun_array[i].conservativeResize(iAfun_array[i].size()+num_qsc_pts);
      jAvar_array[i].conservativeResize(jAvar_array[i].size()+num_qsc_pts);
      A_array[i].conservativeResize(A_array[i].size()+num_qsc_pts);
      for(int k=0;k<nq+num_qsc_pts;k++)
      {
        iCfun_array[i](nG_array[i]+2*k) = nc_array[i]+1;
        iCfun_array[i](nG_array[i]+2*k+1) = nc_array[i]+2;
        jCvar_array[i](nG_array[i]+2*k) = k+1;
        jCvar_array[i](nG_array[i]+2*k+1) = k+1;
      }
      iAfun_array[i].tail(num_qsc_pts) = VectorXd::Constant(num_qsc_pts,nc_array[i]+3);
      for(int k = 0;k<num_qsc_pts;k++)
      {
        jAvar_array[i](nA_array[i]+k) = nq+k+1;
      }
      A_array[i].tail(num_qsc_pts) = VectorXd::Ones(num_qsc_pts);
      Cmin_array[i].conservativeResize(Cmin_array[i].size()+3);
      Cmax_array[i].conservativeResize(Cmax_array[i].size()+3);
      Vector3d qsc_lb,qsc_ub;
      Vector2d qsc_lb_tmp;
      Vector2d qsc_ub_tmp;
      qsc_ptr->bounds(qsc_lb_tmp,qsc_ub_tmp);
      qsc_lb.head(2) = qsc_lb_tmp;
      qsc_lb(2) = 1.0;
      qsc_ub.head(2) = qsc_ub_tmp;
      qsc_ub(2) = 1.0;
      Cmin_array[i].tail(3) = qsc_lb;
      Cmax_array[i].tail(3) = qsc_ub;
      nc_array[i] += 3;
      nG_array[i] += 2*(nq+num_qsc_pts);
      nA_array[i] += num_qsc_pts;
      if(debug_mode)
      {
        vector<string> constraint_name;
        qsc_ptr->name(constraint_name);
        Cname_array[i].push_back(constraint_name[0]);
        Cname_array[i].push_back(constraint_name[1]);
        string qsc_weights_cnst_name;
        if(&t[i]!= NULL)
        {
          char qsc_name_buffer[200];
          sprintf(qsc_name_buffer,"quasi static constraint weights at time %7.3f", t[i]);
          qsc_weights_cnst_name = string(qsc_name_buffer);
        }
        else
        {
          char qsc_name_buffer[200];
          sprintf(qsc_name_buffer,"quasi static constraint weights");
          qsc_weights_cnst_name = string(qsc_name_buffer);
        }
        Cname_array[i].push_back(qsc_weights_cnst_name);
      }
    }
    if(mode == 1)
    {
      if(!quasiStaticFlag)
      {
        nx = nq;
      }
      else
      {
        nx = nq+num_qsc_pts;
      }
      nG = nq + nG_array[i];
      integer* iGfun = new integer[nG];
      integer* jGvar = new integer[nG];
      for(int k = 0;k<nq;k++)
      {
        iGfun[k] = 1;
        jGvar[k] = (integer) k+1;
      }
      for(int k = nq;k<nG;k++)
      {
        iGfun[k] = iCfun_array[i][k-nq]+1;
        jGvar[k] = jCvar_array[i][k-nq];
      }
      nF = nc_array[i]+1;

      integer lenA = A_array[i].size();
      integer* iAfun;
      integer* jAvar;
      doublereal* A;
      if(lenA == 0)
      {
        iAfun = NULL;
        jAvar = NULL;
        A = NULL;
      }
      else
      {
        A = new doublereal[lenA];
        iAfun = new integer[lenA];
        jAvar = new integer[lenA];
        for(int k = 0;k<lenA;k++)
        {
          A[k] = A_array[i](k);
          iAfun[k] = iAfun_array[i](k)+1;
          jAvar[k] = jAvar_array[i](k);
        }
      }
      double* xlow = new double[nx];
      double* xupp = new double[nx];
      memcpy(xlow,joint_limit_min.col(i).data(),sizeof(double)*nq);
      memcpy(xupp,joint_limit_max.col(i).data(),sizeof(double)*nq);
      if(quasiStaticFlag)
      {
        for(int k = 0;k<num_qsc_pts;k++)
        {
          xlow[nq+k] = 0.0;
          xupp[nq+k] = 1.0;
        }
      }
      double* Flow = new double[nF];
      double* Fupp = new double[nF];
      Flow[0] = -mxGetInf();
      Fupp[0] = mxGetInf();
      memcpy(&Flow[1],Cmin_array[i].data(),sizeof(double)*nc_array[i]);
      memcpy(&Fupp[1],Cmax_array[i].data(),sizeof(double)*nc_array[i]);
      ti = &t[i];
      q_nom_i = q_nom.col(i);
      double* x = new double[nx];
      memcpy(x,q_seed.col(i).data(),sizeof(double)*nq);
      if(quasiStaticFlag)
      {
        for(int j = 0;j<num_qsc_pts;j++)
        {
          x[nq+j] = 1.0/num_qsc_pts;
        }
      }

      integer minrw,miniw,mincw;
      integer lenrw = 30000, leniw = 50000, lencw = 500;
      doublereal rw[lenrw];
      integer iw[leniw];
      char cw[8*lencw];

      integer Cold = 0, Basis = 1, Warm = 2;
      doublereal *xmul = new doublereal[nx];
      integer    *xstate = new integer[nx];
      for(int j = 0;j<nx;j++)
      {
        xstate[j] = 0;
      }
      doublereal *F      = new doublereal[nF];
      doublereal *Fmul   = new doublereal[nF];
      integer    *Fstate = new integer[nF];
      for(int j = 0;j<nF;j++)
      {
        Fstate[j] = 0;
      }
      doublereal ObjAdd = 0.0;

      integer ObjRow = 1;
      
      integer   nxname = 1, nFname = 1, npname;
      char* xnames = new char[nxname*8];
      char* Fnames = new char[nFname*8];
      char Prob[200];
      char printname[200];
      char specname[200];

      integer iSpecs = -1, spec_len;
      integer iSumm  = -1;
      integer iPrint = -1, prnt_len;

      integer nS, nInf;
      doublereal sInf;

      /*sprintf(specname, "%s","ik.spc");
      sprintf(printname, "%s","ik.out");
      sprintf(Prob,"%s","ik");
      prnt_len = strlen(printname);
      spec_len = strlen(specname);
      npname = strlen(Prob);
      snopenappend_(&iPrint,printname, &INFO[i], prnt_len);*/

      sninit_(&iPrint,&iSumm,cw,&lencw,iw,&leniw,rw,&lenrw,8*500);
      //snfilewrapper_(specname,&iSpecs,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,spec_len,8*lencw);
      char strOpt1[200] = "Derivative option";
      integer DerOpt = 1, strOpt_len = strlen(strOpt1);
      snseti_(strOpt1,&DerOpt,&iPrint,&iSumm,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
      char strOpt2[200] = "Major optimality tolerance";
      strOpt_len = strlen(strOpt2);
      snsetr_(strOpt2,&SNOPT_MajorOptimalityTolerance,&iPrint,&iSumm,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
      char strOpt3[200] = "Major feasibility tolerance";
      strOpt_len = strlen(strOpt3);
      snsetr_(strOpt3,&SNOPT_MajorFeasibilityTolerance,&iPrint,&iSumm,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
      char strOpt4[200] = "Superbasics limit";
      strOpt_len = strlen(strOpt4);
      snseti_(strOpt4,&SNOPT_SuperbasicsLimit,&iPrint,&iSumm,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
      char strOpt5[200] = "Major iterations limit";
      strOpt_len = strlen(strOpt5);
      snseti_(strOpt5,&SNOPT_MajorIterationsLimit,&iPrint,&iSumm,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
      char strOpt6[200] = "Iterations limit";
      strOpt_len = strlen(strOpt6);
      snseti_(strOpt6,&SNOPT_IterationsLimit,&iPrint,&iSumm,&INFO[i],cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
     

      //debug only
      /* 
      double* f = new double[nF];
      double* G = new double[nG];
      model->doKinematics(x);
      IK_cost_fun(x,f[0],G);
      IK_constraint_fun(x,&f[1],&G[nq]);
      mxArray* f_ptr = mxCreateDoubleMatrix(nF,1,mxREAL);
      mxArray* G_ptr = mxCreateDoubleMatrix(nG,1,mxREAL); 
      memcpy(mxGetPr(f_ptr),f,sizeof(double)*(nF));
      memcpy(mxGetPr(G_ptr),G,sizeof(double)*(nG));
      mxSetCell(plhs[0],0,f_ptr);
      mxSetCell(plhs[0],1,G_ptr);

      double* iGfun_tmp = new double[nG];
      double* jGvar_tmp = new double[nG];
      mxArray* iGfun_ptr = mxCreateDoubleMatrix(nG,1,mxREAL);
      mxArray* jGvar_ptr = mxCreateDoubleMatrix(nG,1,mxREAL);
      for(int k = 0;k<nG;k++)
      {
        iGfun_tmp[k] = (double) iGfun[k];
        jGvar_tmp[k] = (double) jGvar[k];
      } 
      memcpy(mxGetPr(iGfun_ptr),iGfun_tmp,sizeof(double)*nG);
      memcpy(mxGetPr(jGvar_ptr),jGvar_tmp,sizeof(double)*nG);
      mxSetCell(plhs[0],2,iGfun_ptr);
      mxSetCell(plhs[0],3,jGvar_ptr);

      mxArray* Fupp_ptr = mxCreateDoubleMatrix(nF,1,mxREAL);
      mxArray* Flow_ptr = mxCreateDoubleMatrix(nF,1,mxREAL);
      memcpy(mxGetPr(Fupp_ptr),Fupp,sizeof(double)*nF);
      memcpy(mxGetPr(Flow_ptr),Flow,sizeof(double)*nF);
      mxSetCell(plhs[0],4,Fupp_ptr);
      mxSetCell(plhs[0],5,Flow_ptr);

      mxArray* xupp_ptr = mxCreateDoubleMatrix(nx,1,mxREAL);
      mxArray* xlow_ptr = mxCreateDoubleMatrix(nx,1,mxREAL);
      memcpy(mxGetPr(xupp_ptr),xupp,sizeof(double)*nx);
      memcpy(mxGetPr(xlow_ptr),xlow,sizeof(double)*nx);
      mxSetCell(plhs[0],6,xupp_ptr);
      mxSetCell(plhs[0],7,xlow_ptr);

      mxArray* iAfun_ptr = mxCreateDoubleMatrix(lenA,1,mxREAL);
      mxArray* jAvar_ptr = mxCreateDoubleMatrix(lenA,1,mxREAL);
      mxArray* A_ptr = mxCreateDoubleMatrix(lenA,1,mxREAL);
      double* iAfun_tmp = new double[lenA];
      double* jAvar_tmp = new double[lenA];
      for(int k = 0;k<lenA;k++)
      {
        iAfun_tmp[k] = (double) iAfun[k];
        jAvar_tmp[k] = (double) jAvar[k];
      }
      memcpy(mxGetPr(iAfun_ptr),iAfun_tmp,sizeof(double)*lenA);
      memcpy(mxGetPr(jAvar_ptr),jAvar_tmp,sizeof(double)*lenA);
      memcpy(mxGetPr(A_ptr),A,sizeof(double)*lenA);
      mxSetCell(plhs[0],8,iAfun_ptr);
      mxSetCell(plhs[0],9,jAvar_ptr);
      mxSetCell(plhs[0],10,A_ptr);
      
      mxArray* nF_ptr = mxCreateDoubleScalar((double) nF);
      mxSetCell(plhs[0],11,nF_ptr);*/
      snopta_
        ( &Cold, &nF, &nx, &nxname, &nFname,
          &ObjAdd, &ObjRow, Prob, snoptIKfun,
          iAfun, jAvar, &lenA, &lenA, A,
          iGfun, jGvar, &nG, &nG,
          xlow, xupp, xnames, Flow, Fupp, Fnames,
          x, xstate, xmul, F, Fstate, Fmul,
          &INFO[i], &mincw, &miniw, &minrw,
          &nS, &nInf, &sInf,
          cw, &lencw, iw, &leniw, rw, &lenrw,
          cw, &lencw, iw, &leniw, rw, &lenrw,
          npname, 8*nxname, 8*nFname,
          8*500, 8*500);
      //snclose_(&iPrint);
      //snclose_(&iSpecs);
      vector<string> Fname(Cname_array[i]);
      if(debug_mode)
      {
        string objective_name("Objective");
        vector<string>::iterator it = Fname.begin();
        Fname.insert(it,objective_name);
      }
      vector<string> infeasible_constraint_vec;
      if(INFO[i] == 13)
      {
        double *ub_err = new double[nF];
        double *lb_err = new double[nF];
        double max_lb_err = -mxGetInf();
        double max_ub_err = -mxGetInf();
        bool *infeasible_constraint_idx = new bool[nF];
        ub_err[0] = -mxGetInf();
        lb_err[0] = -mxGetInf();
        infeasible_constraint_idx[0] = false;
        for(int j = 1;j<nF;j++)
        {
          ub_err[j] = F[j]-Fupp[j];
          lb_err[j] = Flow[j]-F[j];
          if(ub_err[j]>max_ub_err)
            max_ub_err = ub_err[j];
          if(lb_err[j]>max_lb_err)
            max_lb_err = lb_err[j];
          infeasible_constraint_idx[j] = ub_err[j]>5e-5 | lb_err[j] > 5e-5;
        }
        max_ub_err = (max_ub_err>0.0? max_ub_err: 0.0);
        max_lb_err = (max_lb_err>0.0? max_lb_err: 0.0);
        if(max_ub_err+max_lb_err>1e-4)
        {
          INFO[i] = 13;
          if(debug_mode)
          {
            for(int j = 1;j<nF;j++)
            {
              if(infeasible_constraint_idx[j])
              {
                infeasible_constraint_vec.push_back(Fname[j]);
              }
            }
          }
        }
        else
        {
          INFO[i] = 4;
        }
        delete[] ub_err;
        delete[] lb_err;
        delete[] infeasible_constraint_idx;
      }
      memcpy(q_sol.col(i).data(),x,sizeof(double)*nq);
      memcpy(mxGetPr(q_sol_ptr),q_sol.data(),sizeof(double)*nq*nT);
      for(int i = 0;i<nT;i++)
      {
        INFO_tmp[i] = (double) INFO[i];
      }
      info_ptr = mxCreateDoubleMatrix(1,nT,mxREAL);
      memcpy(mxGetPr(info_ptr),INFO_tmp,sizeof(double)*nT);
       
      mxSetCell(plhs[0],0,q_sol_ptr);
      mxSetCell(plhs[0],1,info_ptr);
      
      mwSize name_dims[1] = {infeasible_constraint_vec.size()};
      mxArray* infeasible_constraint_time_cell = mxCreateCellArray(1,name_dims);
      mxArray *name_ptr;
      for(int j = 0;j<infeasible_constraint_vec.size();j++)
      {
        name_ptr = mxCreateString(infeasible_constraint_vec[j].c_str());
        mxSetCell(infeasible_constraint_time_cell,j,name_ptr);
      }
      if(nT>1)
        mxSetCell(infeasible_constraint_cell,i,infeasible_constraint_time_cell);
      else
        infeasible_constraint_cell = infeasible_constraint_time_cell;
      mxSetCell(plhs[0],2,infeasible_constraint_cell);
      delete[] xmul; delete[] xstate;
      delete[] F; delete[] Fmul; delete[] Fstate;
      delete[] iGfun;  delete[] jGvar;
      if(lenA>0)
      {
        delete[] iAfun;  delete[] jAvar;  delete[] A;
      }
      delete[] x; delete[] xlow; delete[] xupp; delete[] Flow; delete[] Fupp;

    }
  }
  if(mode == 2)
  {
    double* dt = new double[nT-1];
    for(int j = 0;j<nT-1;j++)
    {
      dt[j] = t[j+1]-t[j];
    }
    double* dt_ratio = new double[nT-2];
    for(int j = 0;j<nT-2;j++)
    {
      dt_ratio[j] = dt[j]/dt[j+1];
    }
    pm = mxGetProperty(prhs[ikoptions_idx],0,"Qa");
    Qa.resize(nq,nq);
    memcpy(Qa.data(),mxGetPr(pm),sizeof(double)*nq*nq);
    pm = mxGetProperty(prhs[ikoptions_idx],0,"Qv");
    Qv.resize(nq,nq);
    memcpy(Qv.data(),mxGetPr(pm),sizeof(double)*nq*nq);
    pm = mxGetProperty(prhs[ikoptions_idx],0,"q0");
    q0.resize(nq);
    memcpy(q0.data(),mxGetPr(pm),sizeof(double)*nq);
    pm = mxGetProperty(prhs[ikoptions_idx],0,"qd0");
    qdot0.resize(nq);
    memcpy(qdot0.data(),mxGetPr(pm),sizeof(double)*nq);
    VectorXd qdf_lb(nq);
    VectorXd qdf_ub(nq);
    pm = mxGetProperty(prhs[ikoptions_idx],0,"qdf_lb");
    memcpy(qdf_lb.data(),mxGetPr(pm),sizeof(double)*nq);
    pm = mxGetProperty(prhs[ikoptions_idx],0,"qdf_ub");
    memcpy(qdf_ub.data(),mxGetPr(pm),sizeof(double)*nq);
    VectorXd qdf_seed = (qdf_lb+qdf_ub)/2;
    int nSample = nT-1;
    // This part can be rewritten using the sparse matrix if efficiency becomes a concern
    MatrixXd velocity_mat1 = MatrixXd::Zero(nq*nT,nq*nT);
    MatrixXd velocity_mat2 = MatrixXd::Zero(nq*nT,nq*nT);
    velocity_mat1.block(0,0,nq,nq) = MatrixXd::Identity(nq,nq);
    velocity_mat1.block(nq*(nT-1),nq*(nT-1),nq,nq) = MatrixXd::Identity(nq,nq);
    for(int j = 1;j<nT-1;j++)
    {
      double val_tmp1 = dt[j-1];
      double val_tmp2 = dt[j-1]*(2.0+2.0*dt_ratio[j-1]);
      double val_tmp3 = dt[j-1]*dt_ratio[j-1];
      double val_tmp4 = 3.0-3.0*dt_ratio[j-1]*dt_ratio[j-1];
      double val_tmp5 = 3.0-val_tmp4;
      for(int k = 0;k<nq;k++)
      {
        velocity_mat1(j*nq+k,(j-1)*nq+k) = val_tmp1;
        velocity_mat1(j*nq+k,j*nq+k) = val_tmp2;
        velocity_mat1(j*nq+k,(j+1)*nq+k) = val_tmp3;
        velocity_mat2(j*nq+k,(j-1)*nq+k) = -3.0;
        velocity_mat2(j*nq+k,j*nq+k) = val_tmp4;
        velocity_mat2(j*nq+k,(j+1)*nq+k) = val_tmp5;
      }
    }
    velocity_mat.resize(nq*(nT-2),nq*nT);
    velocity_mat = velocity_mat1.inverse().block(nq,0,nq*(nT-2),nq*nT)*velocity_mat2;

    MatrixXd accel_mat1 = MatrixXd::Zero(nq*nT,nq*nT);
    MatrixXd accel_mat2 = MatrixXd::Zero(nq*nT,nq*nT);
    for(int j = 0;j<nT-1;j++)
    {
      double val_tmp1 = -6.0/(dt[j]*dt[j]);
      double val_tmp2 = -val_tmp1;
      double val_tmp3 = -4.0/dt[j];
      double val_tmp4 = 0.5*val_tmp3;
      for(int k = 0;k<nq;k++)
      {
        accel_mat1(j*nq+k,j*nq+k) = val_tmp1;
        accel_mat1(j*nq+k,(j+1)*nq+k) = val_tmp2;
        accel_mat2(j*nq+k,j*nq+k) = val_tmp3;
        accel_mat2(j*nq+k,(j+1)*nq+k) = val_tmp4;
      }
    }
    for(int k = 0;k<nq;k++)
    {
      double val_tmp1 = 6.0/(dt[nT-2]*dt[nT-2]);
      double val_tmp2 = -val_tmp1;
      double val_tmp3 = 4.0/dt[nT-2];
      double val_tmp4 = 5.0/dt[nT-2];
      accel_mat1((nT-1)*nq+k,(nT-2)*nq+k) = val_tmp1;
      accel_mat1((nT-1)*nq+k,(nT-1)*nq+k) = val_tmp2;
      accel_mat2((nT-1)*nq+k,(nT-2)*nq+k) = val_tmp3;
      accel_mat2((nT-1)*nq+k,(nT-1)*nq+k) = val_tmp4;
    }
    accel_mat.resize(nq*nT,nq*nT);
    accel_mat = accel_mat1+accel_mat2.block(0,nq,nq*nT,nq*(nT-2))*velocity_mat;
    accel_mat_qd0.resize(nq*nT,nq);
    accel_mat_qd0 = accel_mat2.block(0,0,nq*nT,nq);
    accel_mat_qdf.resize(nq*nT,nq);
    accel_mat_qdf = accel_mat2.block(0,(nT-1)*nq,nT*nq,nq);
   
    q_idx = new int[nq*(nT-1)];
    qdotf_idx = new int[nq];
    if(quasiStaticFlag)
    {
      nx= nq*nT+num_qsc_pts*(nT-1);
      for(int j = 0;j<nT-1;j++)
      {
        for(int k = 0;k<nq;k++)
        {
          q_idx[j*nq+k] = j*(nq+num_qsc_pts)+k;
        }
      }
      for(int k = 0;k<nq;k++)
      {
        qdotf_idx[k] = (nT-1)*(nq+num_qsc_pts)+k;
      } 
    }
    else
    {
      nx = nq*nT;
      for(int j = 0;j<(nT-1)*nq;j++)
      {
        q_idx[j] = j;
      }
      for(int j = 0;j<nq;j++)
      {
        qdotf_idx[j] = (nT-1)*nq+j;
      }
    }

    double* xlow = new double[nx];
    double* xupp = new double[nx];
    for(int j = 1;j<nT;j++)
    {
      if(quasiStaticFlag)
      {
        memcpy(xlow+(j-1)*(nq+num_qsc_pts),joint_limit_min.data()+j*nq,sizeof(double)*nq);
        memcpy(xupp+(j-1)*(nq+num_qsc_pts),joint_limit_max.data()+j*nq,sizeof(double)*nq);
        for(int k = 0;k<num_qsc_pts;k++)
        {
          xlow[(j-1)*(nq+num_qsc_pts)+nq+k] = 0.0;
          xupp[(j-1)*(nq+num_qsc_pts)+nq+k] = 1.0;
        }
      }
      else
      {
        memcpy(xlow+(j-1)*nq,joint_limit_min.col(j).data(),sizeof(double)*nq);
        memcpy(xupp+(j-1)*nq,joint_limit_max.col(j).data(),sizeof(double)*nq);
      }
    }
    if(quasiStaticFlag)
    {
      memcpy(xlow+(nq+num_qsc_pts)*(nT-1),qdf_lb.data(),sizeof(double)*nq);
      memcpy(xupp+(nq+num_qsc_pts)*(nT-1),qdf_ub.data(),sizeof(double)*nq);
    }
    else
    {
      memcpy(xlow+nq*(nT-1), qdf_lb.data(),sizeof(double)*nq);
      memcpy(xupp+nq*(nT-1), qdf_ub.data(),sizeof(double)*nq);
    }
    
    nF = 1;
    nG = nq*nT;
    integer lenA = 0;
    for(int j = 1;j<nT;j++)
    {
      nF += nc_array[j]; 
      nG += nG_array[j];
      lenA += nA_array[j];
    } 
    double* Flow = new double[nF];
    double* Fupp = new double[nF];
    string* Fname = new string[nF];
    integer* iGfun = new integer[nG];
    integer* jGvar = new integer[nG];
    integer* iAfun;
    integer* jAvar;
    doublereal* A;
    if(lenA>0)
    {
      iAfun = new integer[lenA];
      jAvar = new integer[lenA];
      A = new doublereal[lenA];
    }
    else
    {
      integer* iAfun = NULL;
      jAvar = NULL;
      A = NULL;
    }
    Flow[0] = -mxGetInf();
    Fupp[0] = mxGetInf();
    Fname[0] = string("Objective");
    for(int j = 0;j<nq*(nT-1);j++)
    {
      iGfun[j] = 1;
      jGvar[j] = q_idx[j]+1;//C interface uses 1 index
    }
    for(int j = 0;j<nq;j++)
    {
      iGfun[j+(nT-1)*nq] = 1;
      jGvar[j+(nT-1)*nq] = qdotf_idx[j]+1;//C interface uses 1 index
    }
    integer nf_cum = 1;
    integer nG_cum = nq*nT;
    integer nA_cum = 0;
    int x_start_idx = 0;
    for(int j = 1;j<nT;j++)
    {
      memcpy(Flow+nf_cum,Cmin_array[j].data(),sizeof(double)*nc_array[j]);
      memcpy(Fupp+nf_cum,Cmax_array[j].data(),sizeof(double)*nc_array[j]);
      for(int k = 0;k<Cname_array[j].size();k++)
      {
        Fname[nf_cum+k] = Cname_array[j][k];
      }
      for(int k = 0;k<nG_array[j];k++)
      {
        iGfun[nG_cum+k] = nf_cum+iCfun_array[j][k];
        jGvar[nG_cum+k] = x_start_idx+jCvar_array[j][k];
      }
      for(int k = 0;k<nA_array[j];k++)
      {
        iAfun[nA_cum+k] = nf_cum+iAfun_array[j][k];
        jAvar[nA_cum+k] = x_start_idx+jAvar_array[j][k];
        A[nA_cum+k] = A_array[j][k];
      }
      nf_cum += nc_array[j];
      nG_cum += nG_array[j];
      nA_cum += nA_array[j];
      if(quasiStaticFlag)
      {
        x_start_idx += nq+num_qsc_pts;
      }
      else
      {
        x_start_idx += nq;
      }
    }
    double* x = new double[nx];
    if(quasiStaticFlag)
    {
      for(int j = 1;j<nT;j++)
      {
        memcpy(x+(j-1)*(nq+num_qsc_pts),q_seed.data()+j*nq,sizeof(double)*nq);
        for(int k = 0;k<num_qsc_pts;k++)
        {
          x[(j-1)*(nq+num_qsc_pts)+nq+k] = 1.0/num_qsc_pts;
        }
      }
      memcpy(x+(nq+num_qsc_pts)*(nT-1),qdf_seed.data(),sizeof(double)*nq);
    }
    else
    {
      memcpy(x,q_seed.data()+nq,sizeof(double)*nq*(nT-1));
      memcpy(x+nq*(nT-1), qdf_seed.data(),sizeof(double)*nq);
    }
    
    integer minrw,miniw,mincw;
    integer lenrw = 100000, leniw = 100000, lencw = 5000;
    doublereal rw[lenrw];
    integer iw[leniw];
    char cw[8*lencw];

    integer Cold = 0, Basis = 1, Warm = 2;
    doublereal *xmul = new doublereal[nx];
    integer    *xstate = new integer[nx];
    for(int j = 0;j<nx;j++)
    {
      xstate[j] = 0;
    }

    doublereal *F      = new doublereal[nF];
    doublereal *Fmul   = new doublereal[nF];
    integer    *Fstate = new integer[nF];
    for(int j = 0;j<nF;j++)
    {
      Fstate[j] = 0;
    }

    doublereal ObjAdd = 0.0;

    integer ObjRow = 1;
    
    integer   nxname = 1, nFname = 1, npname;
    char* xnames = new char[nxname*8];
    char* Fnames = new char[nFname*8];
    char Prob[200];

    integer iSpecs = -1, spec_len;
    integer iSumm = -1;
    integer iPrint = -1, prnt_len;

    integer nS, nInf;
    doublereal sInf;

    sninit_(&iPrint,&iSumm,cw,&lencw,iw,&leniw,rw,&lenrw,8*500);
    char strOpt1[200] = "Derivative option";
    integer DerOpt = 1, strOpt_len = strlen(strOpt1);
    snseti_(strOpt1,&DerOpt,&iPrint,&iSumm,INFO,cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
    char strOpt2[200] = "Major optimality tolerance";
    strOpt_len = strlen(strOpt2);
    snsetr_(strOpt2,&SNOPT_MajorOptimalityTolerance,&iPrint,&iSumm,INFO,cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
    char strOpt3[200] = "Major feasibility tolerance";
    strOpt_len = strlen(strOpt3);
    snsetr_(strOpt3,&SNOPT_MajorFeasibilityTolerance,&iPrint,&iSumm,INFO,cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
    char strOpt4[200] = "Superbasics limit";
    strOpt_len = strlen(strOpt4);
    snseti_(strOpt4,&SNOPT_SuperbasicsLimit,&iPrint,&iSumm,INFO,cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
    char strOpt5[200] = "Major iterations limit";
    strOpt_len = strlen(strOpt5);
    snseti_(strOpt5,&SNOPT_MajorIterationsLimit,&iPrint,&iSumm,INFO,cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
    char strOpt6[200] = "Iterations limit";
    strOpt_len = strlen(strOpt6);
    snseti_(strOpt6,&SNOPT_IterationsLimit,&iPrint,&iSumm,INFO,cw,&lencw,iw,&leniw,rw,&lenrw,strOpt_len,8*500);
    sprintf(Prob,"ik");
    //debug only
    /*mexPrintf("start to debug\n"); 
    double* f = new double[nF];
    double* G = new double[nG];
    IKseq_cost_fun(x,f[0],G);
    mexPrintf("got the cost\n");
    nf_cum = 1;
    nG_cum = nq*nT;
    for(int i = 1;i<nT;i++)
    {
      double* q = x+(i-1)*nq;
      model->doKinematics(q);
      ti = &t[i];
      IK_constraint_fun(q,f+nf_cum,G+nG_cum);
      nf_cum += nc_array[i];
      nG_cum += nG_array[i];
    }
    mxArray* f_ptr = mxCreateDoubleMatrix(nF,1,mxREAL);
    mxArray* G_ptr = mxCreateDoubleMatrix(nG,1,mxREAL); 
    memcpy(mxGetPr(f_ptr),f,sizeof(double)*(nF));
    memcpy(mxGetPr(G_ptr),G,sizeof(double)*(nG));
    mxSetCell(plhs[0],0,f_ptr);
    mxSetCell(plhs[0],1,G_ptr);

    double* iGfun_tmp = new double[nG];
    double* jGvar_tmp = new double[nG];
    mxArray* iGfun_ptr = mxCreateDoubleMatrix(nG,1,mxREAL);
    mxArray* jGvar_ptr = mxCreateDoubleMatrix(nG,1,mxREAL);
    for(int k = 0;k<nG;k++)
    {
      iGfun_tmp[k] = (double) iGfun[k];
      jGvar_tmp[k] = (double) jGvar[k];
    } 
    memcpy(mxGetPr(iGfun_ptr),iGfun_tmp,sizeof(double)*nG);
    memcpy(mxGetPr(jGvar_ptr),jGvar_tmp,sizeof(double)*nG);
    mxSetCell(plhs[0],2,iGfun_ptr);
    mxSetCell(plhs[0],3,jGvar_ptr);

    mxArray* Fupp_ptr = mxCreateDoubleMatrix(nF,1,mxREAL);
    memcpy(mxGetPr(Fupp_ptr),Fupp,sizeof(double)*nF);
    mxSetCell(plhs[0],4,Fupp_ptr);
    mxArray* Flow_ptr = mxCreateDoubleMatrix(nF,1,mxREAL);
    memcpy(mxGetPr(Flow_ptr),Flow,sizeof(double)*nF);
    mxSetCell(plhs[0],5,Flow_ptr);

    mxArray* xupp_ptr = mxCreateDoubleMatrix(nx,1,mxREAL);
    mxArray* xlow_ptr = mxCreateDoubleMatrix(nx,1,mxREAL);
    memcpy(mxGetPr(xupp_ptr),xupp,sizeof(double)*nx);
    memcpy(mxGetPr(xlow_ptr),xlow,sizeof(double)*nx);
    mxSetCell(plhs[0],6,xupp_ptr);
    mxSetCell(plhs[0],7,xlow_ptr);

    mxArray* iAfun_ptr = mxCreateDoubleMatrix(lenA,1,mxREAL);
    mxArray* jAvar_ptr = mxCreateDoubleMatrix(lenA,1,mxREAL);
    mxArray* A_ptr = mxCreateDoubleMatrix(lenA,1,mxREAL);
    double* iAfun_tmp = new double[lenA];
    double* jAvar_tmp = new double[lenA];
    for(int k = 0;k<lenA;k++)
    {
      iAfun_tmp[k] = (double) iAfun[k];
      jAvar_tmp[k] = (double) jAvar[k];
    }
    memcpy(mxGetPr(iAfun_ptr),iAfun_tmp,sizeof(double)*lenA);
    memcpy(mxGetPr(jAvar_ptr),jAvar_tmp,sizeof(double)*lenA);
    memcpy(mxGetPr(A_ptr),A,sizeof(double)*lenA);
    mxSetCell(plhs[0],8,iAfun_ptr);
    mxSetCell(plhs[0],9,jAvar_ptr);
    mxSetCell(plhs[0],10,A_ptr);
    
    mxArray* nF_ptr = mxCreateDoubleScalar((double) nF);
    mxSetCell(plhs[0],11,nF_ptr);*/
    
    snopta_
      ( &Cold, &nF, &nx, &nxname, &nFname,
        &ObjAdd, &ObjRow, Prob, snoptIKseqfun,
        iAfun, jAvar, &lenA, &lenA, A,
        iGfun, jGvar, &nG, &nG,
        xlow, xupp, xnames, Flow, Fupp, Fnames,
        x, xstate, xmul, F, Fstate, Fmul,
        INFO, &mincw, &miniw, &minrw,
        &nS, &nInf, &sInf,
        cw, &lencw, iw, &leniw, rw, &lenrw,
        cw, &lencw, iw, &leniw, rw, &lenrw,
        npname, 8*nxname, 8*nFname,
        8*500, 8*500);
    MatrixXd q(nq*nT,1);
    VectorXd qdotf(nq);
    q.block(0,0,nq,1) = q0;
    //memcpy(q.data()+nq,x,sizeof(double)*nq*(nT-1));
    for(int j = 0;j<nq*(nT-1);j++)
    {
      q(j+nq) = x[q_idx[j]];
    }
    //memcpy(qdotf.data(),x+nq*(nT-1),sizeof(double)*nq);
    for(int j = 0;j<nq;j++)
    {
      qdotf(j) = x[qdotf_idx[j]];
    } 
    MatrixXd qdot(nq*nT,1);
    MatrixXd qddot(nq*nT,1);
    qdot.block(0,0,nq,1) = qdot0;
    qdot.block(nq*(nT-1),0,nq,1) = qdotf;
    qdot.block(nq,0,nq*(nT-2),1) = velocity_mat*q;
    qddot = accel_mat*q+accel_mat_qd0*qdot0+accel_mat_qdf*qdotf;
    q.resize(nq,nT);
    qdot.resize(nq,nT);
    qddot.resize(nq,nT);

    vector<string> infeasible_constraint_vec;
    if(*INFO == 13)
    {
      double *ub_err = new double[nF];
      double *lb_err = new double[nF];
      double max_lb_err = -mxGetInf();
      double max_ub_err = -mxGetInf();
      bool *infeasible_constraint_idx = new bool[nF];
      ub_err[0] = -mxGetInf();
      lb_err[0] = -mxGetInf();
      infeasible_constraint_idx[0] = false;
      for(int j = 1;j<nF;j++)
      {
        ub_err[j] = F[j]-Fupp[j];
        lb_err[j] = Flow[j]-F[j];
        if(ub_err[j]>max_ub_err)
          max_ub_err = ub_err[j];
        if(lb_err[j]>max_lb_err)
          max_lb_err = lb_err[j];
        infeasible_constraint_idx[j] = ub_err[j]>5e-5 | lb_err[j] > 5e-5;
      }
      max_ub_err = (max_ub_err>0.0? max_ub_err: 0.0);
      max_lb_err = (max_lb_err>0.0? max_lb_err: 0.0);
      if(max_ub_err+max_lb_err>1e-4)
      {
        *INFO = 13;
        if(debug_mode)
        {
          for(int j = 1;j<nF;j++)
          {
            if(infeasible_constraint_idx[j])
            {
              infeasible_constraint_vec.push_back(Fname[j]);
            }
          }
        }
      }
      else
      {
        *INFO = 4;
      }
      delete[] ub_err;
      delete[] lb_err;
      delete[] infeasible_constraint_idx;
    }

    mwSize name_dim[1] = {infeasible_constraint_vec.size()};
    mxArray* infeasible_constraint_cell = mxCreateCellArray(1,name_dim);
    for(int j = 0;j<infeasible_constraint_vec.size();j++)
    {
      mxArray* name_ptr = mxCreateString(infeasible_constraint_vec[j].c_str());
      mxSetCell(infeasible_constraint_cell,j,name_ptr);
    }

    memcpy(mxGetPr(q_sol_ptr),q.data(),sizeof(double)*nq*nT);
    mxSetCell(plhs[0],0,q_sol_ptr);
    mxArray* qdot_ptr = mxCreateDoubleMatrix(nq,nT,mxREAL);
    memcpy(mxGetPr(qdot_ptr),qdot.data(),sizeof(double)*nq*nT);
    mxSetCell(plhs[0],1,qdot_ptr);
    mxArray* qddot_ptr = mxCreateDoubleMatrix(nq,nT,mxREAL);
    memcpy(mxGetPr(qddot_ptr),qddot.data(),sizeof(double)*nq*nT);
    mxSetCell(plhs[0],2,qddot_ptr);
    info_ptr = mxCreateDoubleMatrix(1,1,mxREAL);
    *INFO_tmp = (double) *INFO;
    memcpy(mxGetPr(info_ptr),INFO_tmp,sizeof(double));
    mxSetCell(plhs[0],3,info_ptr);
    mxSetCell(plhs[0],4,infeasible_constraint_cell);
    delete[] xmul; delete[] xstate;
    delete[] F; delete[] Fmul; delete[] Fstate;
    delete[] iGfun;  delete[] jGvar;
    if(lenA>0)
    {
      delete[] iAfun;  delete[] jAvar;  delete[] A;
    }
    delete[] x; delete[] xlow; delete[] xupp; delete[] Flow; delete[] Fupp; delete[] Fname;
  } 
  delete[] INFO; delete[] INFO_tmp;
  delete[] iAfun_array; delete[] jAvar_array; delete[] A_array;
  if(mode == 2)
  {
    delete[] q_idx; delete[] qdotf_idx;
  }
  delete[] iCfun_array; delete[] jCvar_array; 
  delete[] Cmin_array; delete[] Cmax_array; delete[] Cname_array;
  delete[] nc_array; delete[] nG_array;
  if(t!=NULL)
  {
    delete[] t;
  }
  delete[] kc_array;
}
