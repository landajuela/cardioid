#include "GradientVoronoiCoarsening.hh"
#include "PerformanceTimers.hh"
#include "pio.h"
#include "ioUtils.h"
#include "Simulate.hh"
using namespace PerformanceTimers;

#include <iostream>
#include <sstream>
#include <iomanip>
#include <list>
using namespace std;

#define GV_DEBUG  0

/////////////////////////////////////////////////////////////////////

static const double tol_det=1.e-8;

double det3(const double a,const double b,const double c,
            const double d,const double e,const double f,
            const double g,const double h,const double i)
{
   return (a*e*i)+(b*f*g)+(c*d*h)-(c*e*g)-(a*f*h)-(b*d*i);
}

// Cramer's rule
int solve3x3(const double s[6], const double r[3], double x[3], const int color)
{
   double det1=det3(s[0],s[1],s[2],s[1],s[3],s[4],s[2],s[4],s[5]);
   if( fabs(det1)<tol_det )
   {
      cout<<"WARNING: Bad condition number for 3x3 system: det="<<det1<<", color="<<color<<endl;
      cout<<"A=["<<s[0]<<" "<<s[1]<<" "<<s[2]<<";"
                 <<s[1]<<" "<<s[3]<<" "<<s[4]<<";"
                 <<s[2]<<" "<<s[4]<<" "<<s[5]<<"]"<<endl;
      //cout<<"r=["<<r[0]<<" "<<r[1]<<" "<<r[2]<<"]"<<endl;
      return 1;
   }
   const double det1i=1./det1;
   x[0]=det3(r[0],s[1],s[2],r[1],s[3],s[4],r[2],s[4],s[5])*det1i;
   x[1]=det3(s[0],r[0],s[2],s[1],r[1],s[4],s[2],r[2],s[5])*det1i;
   x[2]=det3(s[0],s[1],r[0],s[1],s[3],r[1],s[2],s[4],r[2])*det1i;
   
   return 0;
}

/////////////////////////////////////////////////////////////////////

GradientVoronoiCoarsening::GradientVoronoiCoarsening(const SensorParms& sp,
                                     string filename,
                                     const Anatomy& anatomy,
                                     const vector<Long64>& gid,
                                     const PotentialData& vdata,
                                     MPI_Comm comm)
   :Sensor(sp),
    coarsening_(anatomy,gid,comm),
    filename_(filename),
    anatomy_(anatomy),
    vdata_(vdata),
    comm_(comm)
{
   dx_.resize(anatomy.nLocal());
   dy_.resize(anatomy.nLocal());
   dz_.resize(anatomy.nLocal());

   // color local cells
   int ret=coarsening_.bruteForceColoring();
   assert( ret>=0 );
   
   coarsening_.colorDisplacements(dx_,dy_,dz_);

   coarsening_.computeRemoteTasks();
}

void GradientVoronoiCoarsening::computeColorCenterValues(const VectorDouble32& val)
{
   static bool first_time=true;

   // calculate local sums
   const int nLocal = anatomy_.nLocal();
   
   static list<int> indexes;
   static map<int,int> ncellsofcolor;
   
   if( first_time )
   {
      VectorDouble32 values(nLocal,0.);
      for(int ic=0;ic<nLocal;++ic)
      {
         const int color=coarsening_.getColor(ic);
         const double norm2=(dx_[ic]*dx_[ic]
                            +dy_[ic]*dy_[ic]
                            +dz_[ic]*dz_[ic]); 
         if( norm2<1.e-6 ){
            values[ic]=val[ic];
            //cout<<"values["<<ic<<"]="<<values[ic]<<endl;
            indexes.push_back(ic);
         }
         ncellsofcolor[color]++;
      }
      first_time=false;
      
      // calculate local sums
      coarsening_.accumulateValues(values,valcolors_);
   
   }else{
      
      map<int,int>::const_iterator p=ncellsofcolor.begin();
      while(p!=ncellsofcolor.end())
      {
         const int color=p->first;
         
         valcolors_.setSum(color,ncellsofcolor[color],0.);
         p++;
      }
      
      list<int>::const_iterator pi=indexes.begin();
      while(pi!=indexes.end())
      {
         const int ic=*pi;

         int color=coarsening_.getColor(ic);
         
         valcolors_.setSum(color,ncellsofcolor[color],val[ic]);
         
         pi++;
      }
      
   }

   coarsening_.exchangeAndSum(valcolors_);
}
   
// setup least square system dX^T W^2 dX grad V = dX^T W^2 dF
void GradientVoronoiCoarsening::computeLSsystem(const VectorDouble32& val)
{
   // calculate local sums
   valMat00_.clear();
   valMat01_.clear();
   valMat02_.clear();
   valMat11_.clear();
   valMat12_.clear();
   valMat22_.clear();

   valRHS0_.clear();
   valRHS1_.clear();
   valRHS2_.clear();

   const int nLocal = anatomy_.nLocal();
   const double minnorm2=1.e-8;

   for(int ic=0;ic<nLocal;++ic)
   {
      const double norm2=(dx_[ic]*dx_[ic]+dy_[ic]*dy_[ic]+dz_[ic]*dz_[ic]); 
      if( norm2>minnorm2) // skip point at distance 0 
      {
         const int color=coarsening_.getColor(ic);
         const double norm2i=1./norm2; // weighting
         //const double norm2i=1.; 
         valMat00_.add1value(color,dx_[ic]*dx_[ic]*norm2i);
         valMat01_.add1value(color,dx_[ic]*dy_[ic]*norm2i);
         valMat02_.add1value(color,dx_[ic]*dz_[ic]*norm2i);
         valMat11_.add1value(color,dy_[ic]*dy_[ic]*norm2i);
         valMat12_.add1value(color,dy_[ic]*dz_[ic]*norm2i);
         valMat22_.add1value(color,dz_[ic]*dz_[ic]*norm2i);
         
         const double v=norm2i*(val[ic]-valcolors_.value(color));
         valRHS0_.add1value(color,dx_[ic]*v);
         valRHS1_.add1value(color,dy_[ic]*v);
         valRHS2_.add1value(color,dz_[ic]*v);
      }
   }

   vector<LocalSums*> valcolors;
   valcolors.push_back(&valMat00_);
   valcolors.push_back(&valMat01_);
   valcolors.push_back(&valMat02_);
   valcolors.push_back(&valMat11_);
   valcolors.push_back(&valMat12_);
   valcolors.push_back(&valMat22_);
   valcolors.push_back(&valRHS0_);
   valcolors.push_back(&valRHS1_);
   valcolors.push_back(&valRHS2_);
   
   coarsening_.exchangeAndSum(valcolors);
}

void GradientVoronoiCoarsening::writeLeastSquareGradients(const string& filename,
                                                  const double current_time,
                                                  const int current_loop)const
{
   int myRank;
   MPI_Comm_rank(comm_, &myRank);

   PFILE* file = Popen(filename.c_str(), "w", comm_);

   char fmt[] = "%5d %5d %5d %7d %18.12f %18.12f %18.12f";
   int lrec = 84;
   int nfields = 7; 

   static Long64 nSnapSub = -1;

   static bool first_time = true;
   static std::set<int> exclude_colors;
   static int sum_npts=0;
   
   const std::set<int>& owned_colors=coarsening_.getOwnedColors();
   
   if( first_time )
   {
      Long64 nSnapSubLoc = owned_colors.size();
      MPI_Reduce(&nSnapSubLoc, &nSnapSub, 1, MPI_LONG_LONG, MPI_SUM, 0, comm_);

      for(set<int>::const_iterator it = owned_colors.begin();
                                   it!= owned_colors.end();
                                 ++it)
      {
         const int color=(*it);
      
         int ncells=valcolors_.nValues(color);
      
         if( ncells>3 ){
            double a[6]={valMat00_.value(color),
                         valMat01_.value(color),
                         valMat02_.value(color),
                         valMat11_.value(color),
                         valMat12_.value(color),
                         valMat22_.value(color)};
         
            double det1=det3(a[0],a[1],a[2],a[1],a[3],a[4],a[2],a[4],a[5]);
            if( fabs(det1)<tol_det )
            {
               exclude_colors.insert(color);
            }
            
         }else{
            exclude_colors.insert(color);
         }
      } 
      
      first_time = false;
      
      int npts=(int)exclude_colors.size();
      if( npts>0 )cout<<"GradientVoronoiCoarsening --- WARNING: exclude "<<npts<<" points from coarsened data on task "<<myRank<<endl;
      
      MPI_Reduce(&npts, &sum_npts, 1, MPI_INT, MPI_SUM, 0, comm_);
   }

   if (myRank == 0)
   {      
      Long64 nc=nSnapSub-(Long64)sum_npts;
      
      // write header
      int nfiles;
      Pget(file,"ngroup",&nfiles);
      Pprintf(file, "cellViz FILEHEADER {\n");
      Pprintf(file, "  lrec = %d;\n", lrec);
      Pprintf(file, "  datatype = FIXRECORDASCII;\n");
      Pprintf(file, "  nrecords = %llu;\n", nc);
      Pprintf(file, "  nfields = %d;\n", nfields);
      Pprintf(file, "  field_names = rx ry rz nvals GradVmx GradVmy GradVmz;\n");
      Pprintf(file, "  field_types = u u u u f f f;\n" );
      Pprintf(file, "  nfiles = %u;\n", nfiles);
      Pprintf(file, "  time = %f; loop = %u;\n", current_time, current_loop);
      Pprintf(file, "  h = %4u  0    0\n", anatomy_.nx());
      Pprintf(file, "        0    %4u  0\n", anatomy_.ny());
      Pprintf(file, "        0    0    %4u;\n", anatomy_.nz());
      Pprintf(file, "}\n\n");
   }
   
   char line[lrec+1];
   const int halfNx = anatomy_.nx()/2;
   const int halfNy = anatomy_.ny()/2;
   const int halfNz = anatomy_.nz()/2;
   
#if GV_DEBUG
   int ncolors=0;
#endif
   for(set<int>::const_iterator it = owned_colors.begin();
                                it!= owned_colors.end();
                              ++it)
   {
      const int color=(*it);
      
      const std::set<int>::const_iterator itn=exclude_colors.find(color);
      if( itn!=exclude_colors.end() )continue;
      
      //startTimer(sensorEvalTimer);
      
      //cout<<"color="<<color<<endl;
      const Vector& v = coarsening_.center(color);
      int ix = int(v.x()) - halfNx;
      int iy = int(v.y()) - halfNy;
      int iz = int(v.z()) - halfNz;
      
      int ncells=valcolors_.nValues(color);
      
      double g[3]={0.,0.,0.};             
      
      if( ncells>3 ){
         double a[6]={valMat00_.value(color),
                      valMat01_.value(color),
                      valMat02_.value(color),
                      valMat11_.value(color),
                      valMat12_.value(color),
                      valMat22_.value(color)};
         double b[3]={valRHS0_.value(color),
                      valRHS1_.value(color),
                      valRHS2_.value(color)};
         double norm2b=b[0]*b[0]+b[1]*b[1]+b[2]*b[2];          
         
         if( norm2b>1.e-15){
            int ret=solve3x3(a,b,g,color);
            if (ret==1){
               cout<<"WARNING: unable to compute gradient because cells ("<<ncells<<") associated to gid "<<color<<" seem to be in 2D plane!!"<<endl;
            }
         }
      }else{
          cout<<"WARNING: Not enough cells ("<<ncells<<") associated to gid "<<color<<" to compute gradient!!"<<endl;
      } 
      //stopTimer(sensorEvalTimer);

      int l = snprintf(line, lrec, fmt,
                       ix, iy, iz,
                       valcolors_.nValues(color),
                       //valcolors_.value(color),
                       g[0],g[1],g[2]);
#if GV_DEBUG
      ncolors+=valcolors_.nValues(color);
#endif
      
      if (l>=lrec ){
         cerr<<"ERROR: printed record truncated in file "<<filename<<endl;
         cerr<<"This could be caused by out of range values"<<endl;
         cerr<<"l="<<l<<", lrec="<<lrec<<", record="<<line<<endl;
         break;
      }
      for (; l < lrec - 1; l++) line[l] = (char)' ';
      line[l++] = (char)'\n';
      assert (l==lrec);
      const short m=coarsening_.multiplicity(color);
      for(short ii=0;ii<m;ii++)
         Pwrite(line, lrec, 1, file);
   }
   
#if GV_DEBUG
   int ntotal;
   MPI_Reduce(&ncolors, &ntotal, 1, MPI_INT, MPI_SUM, 0, comm_);
   if (myRank == 0)assert( ntotal==anatomy_.nGlobal() );
#endif
   
   Pclose(file);
}

void GradientVoronoiCoarsening::eval(double time, int loop)
{
   startTimer(sensorEvalTimer);
   
   computeColorCenterValues(vdata_.VmArray_);
   computeLSsystem(vdata_.VmArray_);
   
   stopTimer(sensorEvalTimer);
}

void GradientVoronoiCoarsening::print(double time, int loop)
{
   startTimer(sensorPrintTimer);
   
   int myRank;
   MPI_Comm_rank(comm_, &myRank);

   stringstream name;
   name << "snapshot."<<setfill('0')<<setw(12)<<loop;
   string fullname = name.str();
   if (myRank == 0)
      DirTestCreate(fullname.c_str());
   fullname += "/" + filename_;
   
   writeLeastSquareGradients(fullname,time, loop);   

   stopTimer(sensorPrintTimer);
}

