#include "MeshAdaptPUMI.h"
#include <PCU.h>
#include <petscksp.h>

#include <apf.h>
#include <apfMesh.h>
#include <apfShape.h>
#include <apfDynamicMatrix.h>
#include <apfNumbering.h>

#include <iostream>
#include <fstream>

#define  PSR_IDX  0
#define  VEX_IDX  1
#define  VEY_IDX  2
#define  VEZ_IDX  3
#define  VOF_IDX  4
#define  PHI_IDX  5

//proxy variables used to make it easier to pass these variables from MeshAdaptPUMIDrvr
int approx_order;
int int_order;
double nu_0,nu_1,rho_0,rho_1;
double a_kl = 0.5; //flux term weight


//used to attach error estimates to nodes
static void averageToEntity(apf::Field* ef, apf::Field* vf, apf::MeshEntity* ent) //taken from Dan's superconvergent patch recovery code
{
  apf::Mesh* m = apf::getMesh(ef);
  apf::Adjacent elements;
  m->getAdjacent(ent, m->getDimension(), elements);
  double s=0;
  for (std::size_t i=0; i < elements.getSize(); ++i)
    s += apf::getScalar(ef, elements[i], 0);
  s /= elements.getSize();
  apf::setScalar(vf, ent, 0, s);
  return;
}

static void extractFields(apf::Field* solution, apf::Field* pref,apf::Field* velf,apf::Field* voff)
{
  apf::Mesh* m = apf::getMesh(solution);
  apf::MeshIterator* it = m->begin(0);
  apf::MeshEntity* v;
  apf::NewArray<double> tmp(apf::countComponents(solution));
  while ((v = m->iterate(it))) {
    apf::getComponents(solution, v, 0, &tmp[0]);
    apf::setScalar(pref,v,0,tmp[PSR_IDX]); //pressure
    double vel[3] = {tmp[VEX_IDX],tmp[VEY_IDX],tmp[VEZ_IDX]};
    apf::setVector(velf,v,0,&vel[0]);
    double vof = tmp[VOF_IDX];
    apf::setScalar(voff, v, 0, vof);
  }
  m->end(it);
  return;
}

void getProps(double*rho,double*nu)
{
  rho_0 = rho[0];
  nu_0 = nu[0];      
  rho_1 = rho[1];
  nu_1 = nu[1];

  //debug
  rho_1 = rho_0;
  nu_1 = nu_0; 
  
  return;
}

double getMPvalue(double field_val,double val_0, double val_1)
{
  return val_0*(1-field_val)+val_1*field_val;
}

//apf::Vector3 getBoundaryFlux(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf);
//void getBoundaryFlux(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf,apf::NewArray <double> &endflux);
void getBoundaryFlux(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf, double * endflux);
bool isInTet(apf::Mesh* mesh, apf::MeshEntity* elem, apf::Vector3 pt);
apf::Vector3 getFaceNormal(apf::Mesh* mesh, apf::MeshEntity* face);
double getL2error(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf);
double getStarerror(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf, apf::Field* estimate);
double a_k(apf::Matrix3x3 u, apf::Matrix3x3 v,double nu);
double b_k(double a, apf::Matrix3x3 b);
double c_k(apf::Vector3 a, apf::Matrix3x3 b, apf::Vector3 c);
double getDotProduct(apf::Matrix3x3 a, apf::Matrix3x3 b);

void MeshAdaptPUMIDrvr::get_local_error() 
//This function aims to compute error at each element via ERM.
//First get the mesh and impose a 2nd order field
//Then get the desired quadrature points
{

  getProps(rho,nu);
  approx_order = approximation_order; 
  int_order = integration_order;

  //***** Get Solution Fields First *****//
  apf::Field* voff = apf::createLagrangeField(m,"proteus_vof",apf::SCALAR,1);
  apf::Field* velf = apf::createLagrangeField(m,"proteus_vel",apf::VECTOR,1);
  apf::Field* pref = apf::createLagrangeField(m,"proteus_pre",apf::SCALAR,1);
  apf::Field* visc = apf::createLagrangeField(m,"viscosity",apf::SCALAR,1);
  extractFields(solution,pref,velf,voff);
  //*****               *****//
  
  //***** Compute the viscosity field *****//
  apf::Mesh*m = apf::getMesh(solution); 
  apf::MeshEntity* ent;
  apf::MeshIterator* iter = m->begin(0);
  double vof_val, visc_val;
  int nsd = m->getDimension();
  while(ent = m->iterate(iter)){ //loop through all elements
    vof_val=apf::getScalar(voff,ent,0);
    visc_val = getMPvalue(vof_val,nu_0, nu_1);
    apf::setScalar(visc, ent, 0,visc_val);
  }
  m->end(iter);

  //Initialize the Error Fields
  err_reg = apf::createField(m,"ErrorRegion",apf::SCALAR,apf::getConstant(nsd));
  apf::Field* err_vtx = apf::createLagrangeField(m,"error_vtx",apf::SCALAR,1);  //for contraction

  //Start computing element quantities
  int numqpt; //number of quadrature points
  int nshl; //number of local shape functions
  int elem_type; //what type of topology
  double weight; //value container for the weight at each qpt
  double Jdet;
  int numcomps = apf::countComponents(solution);
  //apf::FieldShape* err_shape = apf::getLagrange(approx_order);
  //apf::FieldShape* err_shape = apf::getSerendipity();
  apf::FieldShape* err_shape = apf::getHierarchic();
  apf::Field * estimate = apf::createField(m, "err_est",apf::VECTOR,apf::getHierarchic());
  apf::EntityShape* elem_shape;
  apf::Vector3 qpt; //container for quadrature points
  apf::MeshElement* element;
  apf::Element* visc_elem, *pres_elem,*velo_elem;
  apf::Element* est_elem;
  apf::Matrix3x3 J; //actual Jacobian matrix
  apf::Matrix3x3 invJ; //inverse of Jacobian
  apf::NewArray <double> shpval; //array to store shape function values at quadrature points
  apf::NewArray <double> shpval_temp; //array to store shape function values at quadrature points temporarily
  apf::NewArray <apf::Vector3> shgval; //array to store shape function values at quadrature points

  apf::DynamicMatrix invJ_copy;
  apf::NewArray <apf::DynamicVector> shdrv;
  apf::NewArray <apf::DynamicVector> shgval_copy;
  
  iter = m->begin(nsd); //loop over elements
int testcount = 0;
int eID = 258;//860; 
double effectivity_avg=0.0;

double L2_total=0;
double star_total=0;
double err_est = 0;
double err_est_total=0;
  while(ent = m->iterate(iter)){ //loop through all elements
    element = apf::createMeshElement(m,ent);
    pres_elem = apf::createElement(pref,element);
    velo_elem = apf::createElement(velf,element);
  
    numqpt=apf::countIntPoints(element,int_order); //generally p*p maximum for shape functions
    elem_type = m->getType(ent);
    if(elem_type != m->TET){
      std::cout<<"Not a Tet present"<<std::endl;
      exit(0); 
    }
    nshl=apf::countElementNodes(err_shape,elem_type);
    shgval.allocate(nshl);
    shpval_temp.allocate(nshl);

    int hier_off = 4; //there is an offset that needs to be made to isolate the hierarchic edge modes
    nshl = nshl - hier_off;

    if(testcount==eID && comm_rank==0)
      std::cout<<"nshls "<<nshl<<" numqpt "<<numqpt<<std::endl;
    shpval.allocate(nshl);   shgval_copy.allocate(nshl); shdrv.allocate(nshl);

    //LHS Matrix Initialization
    int ndofs = nshl*nsd;
    Mat K; //matrix size depends on nshl, which may vary from element to element
    MatCreate(PETSC_COMM_SELF,&K);
    MatSetSizes(K,ndofs,ndofs,ndofs,ndofs);
    MatSetFromOptions(K);
    MatSetUp(K); //is this inefficient? check later


    //RHS Vector Initialization
    Vec F;
    VecCreate(PETSC_COMM_SELF,&F);
    VecSetSizes(F,ndofs,ndofs);
    VecSetUp(F);

    //loop through all qpts
    for(int k=0;k<numqpt;k++){
      apf::getIntPoint(element,int_order,k,qpt); //get a quadrature point and store in qpt
      apf::getJacobian(element,qpt,J); //evaluate the Jacobian at the quadrature point
      J = apf::transpose(J); //Is PUMI still defined in this way?
      invJ = invert(J);
      Jdet=apf::getJacobianDeterminant(J,nsd); 
      weight = apf::getIntWeight(element,int_order,k);
      invJ_copy = apf::fromMatrix(invJ);

      //first get the shape function values for error shape functions
      elem_shape = err_shape->getEntityShape(elem_type);
      elem_shape->getValues(NULL,NULL,qpt,shpval_temp);
      elem_shape->getLocalGradients(NULL,NULL,qpt,shgval); 

      for(int i =0;i<nshl;i++){ //get the true derivative and copy only the edge modes for use
        shgval_copy[i] = apf::fromVector(shgval[i+hier_off]);
        shpval[i] = shpval_temp[i+hier_off];
        apf::multiply(shgval_copy[i],invJ_copy,shdrv[i]); 
      }

      //Debugging Information
      //if(testcount==eID) std::cout<<"Local shape gradients "<<shgval[0]<<" "<<shgval[1]<<" "<<shgval[2]<<" "<<shgval[3]<< std::endl;
      //if(testcount==eID) std::cout<<"Global shape gradients "<<shdrv[0]<<" "<<shdrv[1]<<" "<<shdrv[2]<<" "<<shdrv[3]<<" "<<shdrv[4]<<" "<<shdrv[5]<<std::endl;
      //if(testcount==eID) std::cout<<std::scientific<<std::setprecision(15)<< "qpt #"<< k << " value "<<qpt<<std::endl;
      apf::Adjacent dbg_vadj;
      m->getAdjacent(ent,0,dbg_vadj);
      if(testcount==eID && k==0){
         std::cout<<"adjacent verts ";
         apf::Vector3 testpt;
         for(int test_count=0;test_count<4;test_count++){
           m->getPoint(dbg_vadj[test_count],0,testpt);
           std::cout<<testpt<<" ";
          }
          std::cout<<std::endl;
      }
      if(testcount==eID) std::cout<<"Jacobian "<<J<<std::endl;
      if(testcount==eID) std::cout<<"Jdet "<<Jdet<<std::endl;
      if(testcount==eID) std::cout<<"invJ "<<invJ<<std::endl;
      if(testcount==eID) std::cout<<"Shape function"<<shpval[0]<<" "<<shpval[1]<<" "<<shpval[2]<<" "<<shpval[3]<<" "<<shpval[4]<<" "<<shpval[5]<<std::endl;
      if(testcount==eID) std::cout<<"visc_val "<<visc_val<<std::endl;
      if(testcount==eID) std::cout<<"weight "<<weight<<std::endl;
      
       
      //obtain viscosity value
      visc_elem = apf::createElement(visc,element); //at vof currently
      visc_val = apf::getScalar(visc_elem,qpt);

      //Left-Hand Side
      PetscScalar term1[nshl][nshl], term2[nshl][nshl];

      //Calculate LHS Diagonal Block Term
      for(int s=0; s<nshl;s++){
        for(int t=0; t<nshl;t++){
          double temp=0;
          for(int j=0;j<nsd;j++){
            temp+=shdrv[s][j]*shdrv[t][j];
          }
          term1[s][t] = temp*weight*visc_val;
        }
      } 
      int idx[nshl]; //indices for PETSc Mat insertion
      for(int i=0; i<nsd;i++){
        for(int j=0;j<nshl;j++){
          idx[j] = i*nshl+j;
        }
        MatSetValues(K,nshl,idx,nshl,idx,term1[0],ADD_VALUES);
      }

      int idxr[nshl],idxc[nshl]; //indices for PETSc rows and columns
      for(int i = 0; i< nsd;i++){
        for(int j=0; j< nsd;j++){
          for(int s=0;s<nshl;s++){
            for(int t=0;t<nshl;t++){
              term2[s][t] = shdrv[s][j]*shdrv[t][i]*weight*visc_val;
            }
          }
          for(int count=0;count<nshl;count++){
            idxr[count] = i*nshl+count;
            idxc[count] = j*nshl+count;
          }
          MatSetValues(K,nshl,idxr,nshl,idxc,term2[0],ADD_VALUES);
        }
      } //end 2nd term loop

      //Get RHS
      apf::Vector3 vel_vect;
      apf::Matrix3x3 grad_vel;
    
      apf::Element* vof_elem = apf::createElement(voff,element); //at vof currently

      apf::getVector(velo_elem,qpt,vel_vect);
      apf::getVectorGrad(velo_elem,qpt,grad_vel);
      //vector gradient is given in the transpose of the usual definition, need to retranspose it
      grad_vel = apf::transpose(grad_vel);
      if(testcount==eID){
        apf::Vector3 xyz;
        apf::mapLocalToGlobal(element,qpt,xyz);
        std::cout<<"Local "<<qpt<<" Global "<<xyz<<std::endl;
        std::cout<<"Velocity "<<vel_vect<<" Gradient "<<grad_vel<<std::endl;
        std::cout<<"Pressure "<<apf::getScalar(pres_elem,qpt)<<std::endl;
      }

      for( int i = 0; i<nsd; i++){
        double temp_vect[nshl];
        for( int s=0;s<nshl;s++){
          idx[s] = i*nshl+s;

          //forcing term
          temp_vect[s] = (0)*shpval[s];
          //a(u,v) and c(u,u,v) term
          for(int j=0;j<nsd;j++){
            temp_vect[s] += -visc_val*shdrv[s][j]*(grad_vel[i][j]+grad_vel[j][i]);
            temp_vect[s] += -shpval[s]*grad_vel[i][j]*vel_vect[j];
          }
          //need to scale pressure by density b(p,v)
          temp_vect[s] += apf::getScalar(pres_elem,qpt)/getMPvalue(apf::getScalar(vof_elem,qpt),rho_0,rho_1)*shdrv[s][i]; //pressure term

          temp_vect[s] = temp_vect[s]*weight;
        } //end loop over number of shape functions
        VecSetValues(F,nshl,idx,temp_vect,ADD_VALUES);
      } //end loop over spatial dimensions
    } //end loop over quadrature

    //to complete integration, scale by the determinant of the Jacobian

    MatAssemblyBegin(K,MAT_FINAL_ASSEMBLY);
    MatAssemblyEnd(K,MAT_FINAL_ASSEMBLY);
    MatScale(K,Jdet); //must be done after assembly
    VecAssemblyBegin(F);
    VecAssemblyEnd(F); VecScale(F,Jdet); //must be done after assembly
 
if(comm_rank==0 && testcount==eID){ 
      MatView(K,PETSC_VIEWER_STDOUT_SELF);
      std::cout<<" NOW VECTOR with just a(.,.)" <<std::endl;
      VecView(F,PETSC_VIEWER_STDOUT_SELF);
}
   
    double* bflux;
    int F_idx[ndofs];
    bflux = (double*) calloc(ndofs,sizeof(double));
/*
    if(testcount==eID){ getBoundaryFlux(m,ent,voff,visc,pref,velf,bflux); 
    std::cout<<"Bflux Result "<<std::endl;
    for(int s=0;s<ndofs;s++) std::cout<<bflux[s]<<std::endl;
    }
*/
    getBoundaryFlux(m,ent,voff,visc,pref,velf,bflux);
    for(int s=0;s<ndofs;s++){
      F_idx[s]=s;
    }
    VecSetValues(F,ndofs,F_idx,bflux,ADD_VALUES);
    VecAssemblyBegin(F); VecAssemblyEnd(F);
    free(bflux);
    Vec coef;
    VecCreate(PETSC_COMM_SELF,&coef);
    VecSetSizes(coef,ndofs,ndofs);
    VecSetUp(coef);

//    if(testcount==eID && comm_rank==0){

//Save Temporarily for Debugging
/*
      std::ofstream myfile ("stiffness.csv");
      std::ofstream myfile2 ("force.csv");
      std::ofstream myfilegsl("stiffness.txt");
      myfile<<std::scientific<<std::setprecision(15);
      myfile2<<std::scientific<<std::setprecision(15);
      myfilegsl<<std::scientific<<std::setprecision(15);
      PetscScalar matstor;  
      PetscScalar vecstor;  
      int idxr[ndofs], idxc[ndofs];
      for(int ii=0;ii<ndofs;ii++){
        idxr[ii]=ii;
        for(int jj=0;jj<ndofs;jj++){
          idxc[jj]=jj;
          MatGetValues(K,1,&idxr[ii],1,&idxc[jj],&matstor);
          myfile<<matstor<<","; 
          myfilegsl<<matstor<<std::endl;
        }
        myfile<<std::endl;
        VecGetValues(F,1,&idxr[ii],&vecstor);
        myfile2<<vecstor<<std::endl;
      }
      myfile.close();
*/
    //MatView(K,PETSC_VIEWER_STDOUT_SELF);
    //VecView(F,PETSC_VIEWER_STDOUT_SELF);

    KSP ksp; //initialize solver context
    KSPCreate(PETSC_COMM_SELF,&ksp);
    KSPSetOperators(ksp,K,K);
    KSPSetType(ksp,KSPPREONLY);
    PC pc;
    //PCSetOperators(pc,K,K);
    KSPGetPC(ksp,&pc);
    PCSetType(pc,PCLU);
    KSPSetFromOptions(ksp);

    std::cout<<"Final error "<<std::endl;
    KSPSolve(ksp,F,coef);
    //VecView(coef,PETSC_VIEWER_STDOUT_SELF);
    
    KSPDestroy(&ksp); //destroy ksp
    PCDestroy(&pc);

    //compute the local error  
    double Acomp=0;
    double Bcomp=0;
    apf::Matrix3x3 phi_ij;


    double coef_ez[nshl*nsd];
    int ez_idx[nshl*nsd];
    for(int ez=0;ez<nshl*nsd;ez++){ez_idx[ez]=ez;}
    VecGetValues(coef,nshl*nsd,ez_idx,coef_ez);

    //Copy coefficients onto field
    //apf::MeshEntity* testedge;
    apf::Adjacent adjvert;
    m->getAdjacent(ent,0,adjvert);
    for(int idx=0;idx<4;idx++){
      double coef_sub[3]={0,0,0};
      apf::setVector(estimate,adjvert[idx],0,&coef_sub[0]);
    }
    
    apf::Adjacent adjedg;
    m->getAdjacent(ent,1,adjedg);
    for(int idx=0;idx<nshl;idx++){
      double coef_sub[3] ={coef_ez[idx],coef_ez[nshl+idx],coef_ez[nshl*2+idx]};
      apf::setVector(estimate,adjedg[idx],0,&coef_sub[0]);
    }

    est_elem= apf::createElement(estimate,element);   
    for(int k=0; k<numqpt;k++){ 
      apf::getIntPoint(element,int_order,k,qpt); //get a quadrature point and store in qpt
      apf::getJacobian(element,qpt,J); //evaluate the Jacobian at the quadrature point

      invJ = invert(J);
      invJ = apf::transpose(invJ);
      Jdet=apf::getJacobianDeterminant(J,nsd); 
      weight = apf::getIntWeight(element,int_order,k);
      invJ_copy = apf::fromMatrix(invJ);

      //first get the shape function values for error shape functions
      elem_shape = err_shape->getEntityShape(elem_type);
      elem_shape->getValues(NULL,NULL,qpt,shpval_temp);
      elem_shape->getLocalGradients(NULL,NULL,qpt,shgval); 

      for(int i =0;i<nshl;i++){ //get the true derivative and copy only the edge modes for use
        shgval_copy[i] = apf::fromVector(shgval[i+hier_off]);
        shpval[i] = shpval_temp[i+hier_off];
        apf::multiply(shgval_copy[i],invJ_copy,shdrv[i]); 
      }
      visc_val = apf::getScalar(visc_elem,qpt);
      apf::getVectorGrad(est_elem,qpt,phi_ij);
      phi_ij = apf::transpose(phi_ij);
      Acomp = Acomp + visc_val*getDotProduct(phi_ij,phi_ij+apf::transpose(phi_ij))*weight;
      Bcomp = Bcomp + apf::getDiv(velo_elem,qpt)*apf::getDiv(velo_elem,qpt)*weight;
    } //end compute local error

    Acomp = Acomp*Jdet; //Jacobian+nondimensionalize
    Bcomp = Bcomp*Jdet;
    err_est = sqrt(Acomp+Bcomp); //the square root should be here because the local error is given by this. but for statistics it's necessary for it to not be square rooted
    apf::setScalar(err_reg,ent,0,err_est);
    err_est_total = err_est_total+(Acomp+Bcomp); //for tracking the upper bound
    double L2err= getL2error(m,ent,voff,visc,pref,velf); //non-dimensional
    L2_total = L2_total+L2err;
    double starerr = getStarerror(m,ent,voff,visc,pref,velf,estimate);
    star_total = star_total+starerr;
std::cout<<"Acomp "<<Acomp << " Bcomp "<<Bcomp<<std::endl;
   
//    } //end if testcount 

//   apf::setScalar(err_reg,ent,0,Jdet); //temporary place in
    MatDestroy(&K); //destroy the matrix
    VecDestroy(&F); //destroy vector
    VecDestroy(&coef); //destroy vector


testcount++;
  } //end element loop
star_total = -2*(0.5*(err_est_total)-star_total); //before square root is taken
err_est_total = sqrt(err_est_total);
L2_total = sqrt(L2_total);
if(star_total<0){ star_total=star_total*-1;std::cout<<"star err Was negative "<<std::endl;}
star_total = sqrt(star_total);
std::cout<<"Err_est "<<err_est_total<<" L2 "<<L2_total<<" Average "<<err_est_total/L2_total<<std::endl;
std::cout<<"Err_est "<<err_est_total<<" star "<<star_total<<" Average "<<err_est_total/star_total<<std::endl;
  m->end(iter);

  //store error field onto vertices
  apf::MeshIterator* iter_vtx = m->begin(0);
  while(ent = m->iterate(iter_vtx)){
    averageToEntity(err_reg, err_vtx, ent);
  }
  m->end(iter_vtx);
  getERMSizeField(err_est_total);
  apf::destroyElement(visc_elem);apf::destroyElement(pres_elem);apf::destroyElement(velo_elem);apf::destroyElement(est_elem);
  apf::destroyField(voff);  apf::destroyField(visc); apf::destroyField(velf); apf::destroyField(pref); apf::destroyField(estimate);
//  m->destroyTag(fluxtag[1]); m->destroyTag(fluxtag[2]); m->destroyTag(fluxtag[3]);
  freeField(fluxBC);
  printf("It cleared the function.\n");
}

void getBoundaryFlux(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf, double * endflux){

    int nsd = m->getDimension();
    int nshl;
    apf::NewArray <double> shpval;
    apf::NewArray <double> shpval_temp;

    //apf::FieldShape* err_shape = apf::getLagrange(approx_order);
    apf::FieldShape* err_shape = apf::getHierarchic();
    apf::EntityShape* elem_shape;

    //loop over element faces
    apf::Adjacent boundaries;
    apf::Adjacent neighbors;
    apf::MeshEntity* bent;
    apf::MeshElement* b_elem;
    apf::Vector3 bqpt,bqptl,bqptshp;

    double weight, Jdet;
    apf::Matrix3x3 J;
    apf::Vector3 normal;
    apf::Vector3 centerdir;

    //Shape functions of the region and not the boundaries
    nshl=apf::countElementNodes(err_shape,m->getType(ent));
    shpval_temp.allocate(nshl);
    int hier_off = 4;
    nshl= nshl-hier_off;
    shpval.allocate(nshl);
    elem_shape = err_shape->getEntityShape(m->getType(ent));
  
    m->getAdjacent(ent,nsd-1,boundaries);
    for(int adjcount =0;adjcount<boundaries.getSize();adjcount++){

      apf::Vector3 bflux(0.0,0.0,0.0); 
      apf::Matrix3x3 tempbflux(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);
      bent = boundaries[adjcount];

      apf::ModelEntity* me=m->toModel(bent);
      int tag = m->getModelTag(me);
      apf::ModelEntity* boundary_face = m->findModelEntity(nsd-1,tag);
        if(m->isShared(bent)){//is shared by a parallel entity
          std::cout<<"PARALLEL "<<std::endl;
        }
        else{
          m->getAdjacent(bent,nsd,neighbors);
          b_elem = apf::createMeshElement(m,bent);
          apf::Matrix3x3 tempgrad_velo[2];
          apf::Matrix3x3 identity(1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0);
          apf::MeshElement* tempelem; apf::Element * tempvelo,*temppres,*tempvoff;
          
          normal=getFaceNormal(m,bent);
          centerdir=apf::getLinearCentroid(m,ent)-apf::getLinearCentroid(m,bent);
          if(isInTet(m,ent,apf::project(normal,centerdir)*centerdir.getLength()+apf::getLinearCentroid(m,bent)))
            normal = normal*-1.0; //normal needs to face the other direction

          //shape functions are from weighting function and independent of neighbors

          double flux_weight;         
          for(int idx_neigh=0; idx_neigh<neighbors.getSize();idx_neigh++){ //at most two neighboring elements
            if(me==boundary_face){
              flux_weight=1; std::cout<<"on boundary face "<<std::endl;
            }
            else{
              if(neighbors[idx_neigh]==ent) flux_weight = 1-a_kl;
              else{ 
                flux_weight = a_kl;
              }
            }
            tempelem = apf::createMeshElement(m,neighbors[idx_neigh]);
            temppres = apf::createElement(pref,tempelem);
            tempvelo = apf::createElement(velf,tempelem);
            tempvoff = apf::createElement(voff,tempelem);
            for(int l=0; l<apf::countIntPoints(b_elem,int_order);l++){
              apf::getIntPoint(b_elem,int_order,l,bqpt);
              weight = apf::getIntWeight(b_elem,int_order,l);
              apf::getJacobian(b_elem,bqpt,J); //evaluate the Jacobian at the quadrature point
              Jdet=apf::getJacobianDeterminant(J,nsd-1);
              bqptl=apf::boundaryToElementXi(m,bent,neighbors[idx_neigh],bqpt); 
              bqptshp=apf::boundaryToElementXi(m,bent,ent,bqpt); 
              elem_shape->getValues(NULL,NULL,bqptshp,shpval_temp);
              for(int j=0;j<nshl;j++){ shpval[j] = shpval_temp[hier_off+j];}
              
              apf::getVectorGrad(tempvelo,bqptl,tempgrad_velo[idx_neigh]);
              tempbflux = ((tempgrad_velo[idx_neigh]+apf::transpose(tempgrad_velo[idx_neigh]))*getMPvalue(apf::getScalar(tempvoff,bqptl),nu_0,nu_1)
                -identity*apf::getScalar(temppres,bqptl)/getMPvalue(apf::getScalar(tempvoff,bqptl),rho_0,rho_1))*weight*Jdet*flux_weight;
              bflux = tempbflux*normal;

              for(int i=0;i<nsd;i++){ 
                for(int s=0;s<nshl;s++){
                  endflux[i*nshl+s] = endflux[i*nshl+s]+bflux[i]*shpval[s];
                }
              } 
  
            } //end boundary integration loop
          } //end for loop of neighbors

          apf::destroyMeshElement(tempelem);apf::destroyElement(tempvelo);apf::destroyElement(temppres); apf::destroyElement(tempvoff);
        }
    } //end loop over adjacent faces
}//end function


apf::Vector3 getFaceNormal(apf::Mesh* mesh, apf::MeshEntity* face){ //get the normal vector
  apf::Vector3 normal;
  apf::Adjacent verts;
  mesh->getAdjacent(face,0,verts);
  apf::Vector3 vtxs[verts.getSize()]; //4 points
  for(int i=0;i<verts.getSize();i++){
    mesh->getPoint(verts[i],0,vtxs[i]); 
  } 
  apf::Vector3 a,b;
  a = vtxs[1]-vtxs[0];
  b = vtxs[2]-vtxs[0];
  normal = apf::cross(a,b);

  return normal.normalize();
}


double getDotProduct(apf::Vector3 a, apf::Vector3 b){
  return (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]);
}

double getDotProduct(apf::Matrix3x3 a, apf::Matrix3x3 b){
  double temp =0;
  for(int i=0;i<3;i++){
    for(int j=0;j<3;j++){
      temp = temp + a[i][j]*b[i][j];
    }
  }
  return temp;
}


bool isInTet(apf::Mesh* mesh, apf::MeshEntity* ent, apf::Vector3 pt){
  bool isin=0;

  apf::Adjacent verts;
  mesh->getAdjacent(ent,0,verts);
  apf::Vector3 vtxs[4]; //4 points
  for(int i=0;i<4;i++){
    mesh->getPoint(verts[i],0,vtxs[i]); 
  } 
  apf::Vector3 c[4];
  c[0] = vtxs[1]-vtxs[0];
  c[1] = vtxs[2]-vtxs[0];
  c[2] = vtxs[3]-vtxs[0];
  c[3] = pt-vtxs[0];

  apf::Matrix3x3 K,Kinv;
  apf::Vector3 F;
  for(int i=0;i<3;i++){
    for(int j=0;j<3;j++){
      K[i][j] = getDotProduct(c[i],c[j]);
    }
    F[i] = getDotProduct(c[3],c[i]);
  }
  Kinv = invert(K);
  apf::DynamicMatrix Kinv_dyn = apf::fromMatrix(Kinv);
  apf::DynamicVector F_dyn = apf::fromVector(F);
  apf::DynamicVector uvw; //result
  apf::multiply(Kinv_dyn,F_dyn,uvw);
  if(uvw[0] >= 0 && uvw[1] >=0 && uvw[2] >=0 && (uvw[0]+uvw[1]+uvw[2])<=1) isin = 1;
/*
  std::cout<<"Points "<<vtxs[0]<<" "<<vtxs[1]<<" "<<vtxs[2]<<" "<<vtxs[3]<<std::endl;
  std::cout<<"Vectors "<<c[0]<<" "<<c[1]<<" "<<c[2]<<" "<<c[3]<<std::endl;
  std::cout<<"K "<<K<<std::endl;   
  std::cout<<"F "<<F<<std::endl;   
  std::cout<<"Result "<<uvw<<std::endl;   
*/
  return isin;
}

double getL2error(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf){

    int nsd = m->getDimension();
    int nshl; //assuming linear solution
    int numqpt;
    int elem_type;

    apf::FieldShape* err_shape = apf::getLagrange(approx_order);
    apf::EntityShape* elem_shape;
    elem_type = m->getType(ent);

    nshl=apf::countElementNodes(err_shape,elem_type);

    double Lz = 0.05;
    double Ly = 0.2;
    double u_exact, u_h,p_exact,p_h, dpdy;
    double L2_err=0.0; 

    apf::MeshElement* element;
    apf::Element* visc_elem, *pres_elem,*velo_elem;
    double weight, Jdet;
    apf::Matrix3x3 J;
    apf::Vector3 qpt;

    element = apf::createMeshElement(m,ent);
    pres_elem = apf::createElement(pref,element);
    velo_elem = apf::createElement(velf,element);
  
    numqpt=apf::countIntPoints(element,int_order); //generally p*p maximum for shape functions
    apf::Vector3 xyz;
    apf::Vector3 vel_vect;

    for(int k=0;k<numqpt;k++){
      apf::getIntPoint(element,int_order,k,qpt);
      apf::getJacobian(element,qpt,J); 
      Jdet=apf::getJacobianDeterminant(J,nsd); 
      weight = apf::getIntWeight(element,int_order,k);
   
      apf::mapLocalToGlobal(element,qpt,xyz);
      apf::getVector(velo_elem,qpt,vel_vect);

      //Hardcoded Exact Solution    
int casenum = 0;
      if(casenum==0){ 
      //Poiseuille Flow
        dpdy = -1/Ly;
        u_exact= 0.5/(nu_0*rho_0)*(dpdy)*(xyz[2]*xyz[2]-Lz*xyz[2]);
        p_exact = 1+xyz[1]*dpdy;
      }
      else if(casenum ==1){
      //Couette Flow
        u_exact = 1.0*xyz[2]/Lz;
        p_exact = 0;
      }

      u_h = vel_vect[1]; 
      p_h = apf::getScalar(pres_elem,qpt);

      double temp=0.0;
      temp = temp + (u_exact-u_h)*(u_exact-u_h);
      temp = temp + (p_exact-p_h)*(p_exact-p_h)/rho_0/rho_0;
      L2_err = L2_err+temp *weight*Jdet;
   }

  apf::destroyMeshElement(element);apf::destroyElement(velo_elem);apf::destroyElement(pres_elem);
  return L2_err;
}

double getStarerror(apf::Mesh* m, apf::MeshEntity* ent, apf::Field* voff, apf::Field* visc,apf::Field* pref, apf::Field* velf, apf::Field* estimate){

    int nsd = m->getDimension();
    int nshl_err,nshl_est;  //exact error vs estimate
    int numqpt;
    int elem_type;

    apf::FieldShape* err_shape = apf::getLagrange(approx_order);
    apf::FieldShape* est_shape = apf::getHierarchic();
    apf::EntityShape* elem_shape;
    elem_type = m->getType(ent);

    nshl_err=apf::countElementNodes(err_shape,elem_type);
    nshl_est=apf::countElementNodes(est_shape,elem_type);

    double Lz = 0.05;
    double Ly = 0.2;
    //double u_exact, u_h,
    double p_exact,p_h, dpdy,div_u_h;
    double star_err=0.0; 

    apf::MeshElement* element;
    apf::Element* visc_elem, *pres_elem,*velo_elem, *est_elem;
    double weight, Jdet;
    apf::Matrix3x3 J,grad_u_exact,grad_u_h,grad_est;
    apf::Vector3 qpt;

    element = apf::createMeshElement(m,ent);
    pres_elem = apf::createElement(pref,element);
    velo_elem = apf::createElement(velf,element);
    est_elem = apf::createElement(estimate,element);
  
    numqpt=apf::countIntPoints(element,int_order); //generally p*p maximum for shape functions
    apf::Vector3 xyz;
    apf::Vector3 vel_vect;
    apf::Vector3 est_vect;
    apf::Vector3 u_exact, u_h;

    for(int k=0;k<numqpt;k++){
      apf::getIntPoint(element,int_order,k,qpt);
      apf::getJacobian(element,qpt,J); 
      Jdet=apf::getJacobianDeterminant(J,nsd); 
      weight = apf::getIntWeight(element,int_order,k);
   
      apf::mapLocalToGlobal(element,qpt,xyz);
      apf::getVector(velo_elem,qpt,vel_vect);

      //Hardcoded Exact Solution    
int casenum = 0;
      if(casenum==0){ 
      //Poiseuille Flow
        dpdy = -1/Ly;
        u_exact[0]=0;
        u_exact[1]= 0.5/(nu_0*rho_0)*(dpdy)*(xyz[2]*xyz[2]-Lz*xyz[2]);
        u_exact[2]=0;
        p_exact = 1+xyz[1]*dpdy;
        grad_u_exact[1][2] = 0.5/(nu_0*rho_0)*(dpdy)*(2*xyz[2]-Lz);
      }
      else if(casenum ==1){
      //Couette Flow
        u_exact[0]=0;
        u_exact[1] = 1.0*xyz[2]/Lz;
        u_exact[2]=0;
        p_exact = 0;
        grad_u_exact[1][2] = 1.0/Lz;
      }

      u_h = vel_vect; 
      p_h = apf::getScalar(pres_elem,qpt);

      apf::getVector(est_elem,qpt,est_vect); 
      apf::getVectorGrad(est_elem,qpt,grad_est);
      apf::getVectorGrad(velo_elem,qpt,grad_u_h);
      grad_u_h=apf::transpose(grad_u_h);
      div_u_h = grad_u_h[0][0]+grad_u_h[1][1]+grad_u_h[2][2];            

      double temp=0.0;
      temp = temp+a_k(grad_u_exact-grad_u_h,grad_est,nu_0); //nu is hardcoded because it's not necessary to generalize yet
      temp = temp-b_k(p_exact-p_h,grad_est); 
      temp = temp-b_k(div_u_h,grad_u_exact-grad_u_h); 
      temp = temp + c_k(u_exact,grad_u_exact,est_vect);
      temp = temp - c_k(u_h,grad_u_h,est_vect);
if(k==0)
//std::cout<<"C contribution "<<c_k(u_exact,grad_u_exact,est_vect)<<" "<<c_k(u_h,grad_u_h,est_vect)<<" "<<temp<<std::endl;
//std::cout<<"Grad "<<grad_u_h<<std::endl;

      star_err = star_err+temp *weight*Jdet;
   }

  apf::destroyMeshElement(element);apf::destroyElement(velo_elem);apf::destroyElement(pres_elem); apf::destroyElement(est_elem);
  return star_err;
}

double a_k(apf::Matrix3x3 u, apf::Matrix3x3 v,double nu){
  //u and v are gradients of a vector
  apf::Matrix3x3 temp_u = u+apf::transpose(u);
  apf::Matrix3x3 temp_v = v+apf::transpose(v);
  return nu*getDotProduct(temp_u,temp_v);
} 

double b_k(double a, apf::Matrix3x3 b){
  //b is a gradient of a vector
  return a*(b[0][0]+b[1][1]+b[1][1]);
}

double c_k(apf::Vector3 a, apf::Matrix3x3 b, apf::Vector3 c){
  //b is a gradient of a vector
  return getDotProduct(b*a,c);
}
