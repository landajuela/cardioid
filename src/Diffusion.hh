#ifndef DIFFUSION_HH
#define DIFFUSION_HH

#include <vector>

class Diffusion
{
 public:
   virtual ~Diffusion(){};
   virtual void updateLocalVoltage(const double* VmLocal) = 0;
   virtual void updateRemoteVoltage(const double* VmRemote) = 0;
   virtual void calc(std::vector<double>& dVm) = 0;
   virtual unsigned* blockIndex(){return 0;}
   virtual unsigned* blockIndexB(){return blockIndex();}
   virtual double* VmBlock(){return 0;}
   virtual double* dVmBlock(){return 0;}
   virtual double diffusionScale(){return 1;}
   virtual void  dump_VmBlock(int tmp){;}
};

#endif
