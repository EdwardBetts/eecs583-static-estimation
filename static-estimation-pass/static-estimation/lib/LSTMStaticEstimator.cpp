// This pass extracts LSTM features in a similar method to the static estimator pass. 
// Instead of creating a feature vector for each path, however, we create a (simple)
// feature vector for each basic block and will use those BB's as the sequences to the LSTM
#include "llvm/Transforms/Instrumentation.h"
#include "ProfilingUtils.h"
#include "llvm/Analysis/PathNumbering.h"
#include "llvm/Analysis/PathProfileInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <fstream>
#include <vector>
#include <unordered_map>
#include <string>

#include "BLInstrumentation.h"
#include "FeatureExtractor.h"

#define MAX_PATHS 500

using namespace llvm;

class LSTMStaticEstimatorPass : public ModulePass {
private:
  // Profiling
  PathProfileInfo* PI;

  // File for output
  std::ofstream ofs;

  // Instruments each function with path profiling.  'main' is instrumented
  // with code to save the profile to disk.
  bool runOnModule(Module &M);

  // Calculate the path for a single ID
  std::vector<BasicBlock*> computePath(BLInstrumentationDag* dag, unsigned pathNo);

  // Calculates all paths for a dag
  void calculatePaths(BLInstrumentationDag* dag);

  // Analyzes the function for Ball-Larus path profiling, and inserts code.
  void runOnFunction(std::vector<Constant*> &ftInit, Function &F, Module &M);

  // To use profiling info
  void getAnalysisUsage(AnalysisUsage &AU) const;


public:
  static char ID; // Pass identification, replacement for typeid
  LSTMStaticEstimatorPass() : ModulePass(ID) {
    //initializeStaticEstimatorPass(*PassRegistry::getPassRegistry());
  }

  virtual const char *getPassName() const {
    return "LSTM Static Estimation";
  }
};

// Compute the path through the DAG from its path number
std::vector<BasicBlock*> LSTMStaticEstimatorPass::computePath(BLInstrumentationDag* dag, unsigned pathNo) {
    unsigned R = pathNo;
    std::vector<BasicBlock*> path;
    
    // errs() << "\n\n\n\nComputing path number: " << pathNo << "\n";

    int foundNode = 0;
    int breakOut = 0;

   /* 
    BLInstrumentationEdge* prev4 = NULL;
    BLInstrumentationEdge* prev3 = NULL;
    BLInstrumentationEdge* prev2 = NULL;
    BLInstrumentationEdge* prev1 = NULL;
    */
    int found_edge;
    std::unordered_map<BLInstrumentationEdge*,int> taken;

    BLInstrumentationNode* curNode = (BLInstrumentationNode*)(dag->getRoot());
    while (1) {
        BLInstrumentationEdge* nextEdge;
        unsigned bestEdge = 0;
        // Add the basic block to the list
        found_edge = 0;
        path.push_back(curNode->getBlock());
	    //errs() << "next edge options: \n";
        foundNode = 0;

	    for (BLEdgeIterator next = curNode->succBegin(), end = curNode->succEnd(); next != end; next++) {
            // We want the largest edge that's less than R
            BLInstrumentationEdge* i = (BLInstrumentationEdge*) *next;
            /*
            if (i->getTarget()->getBlock() == NULL) 
                errs() << i->getTarget() << " weight: " << i->getWeight() << " exit: "
                   << " name: NULL! \n";
            else 
                errs() << i->getTarget() << " weight: " << i->getWeight() << " exit: " 
                    << " name: " << i->getTarget()->getBlock()->getName().str() << "\n";
           */
            
            unsigned weight = i->getWeight();
            if (weight <= R && weight >= bestEdge && (taken.find(i) == taken.end() || found_edge == 0)) {
	        /*if (weight <= R && weight >= bestEdge && (found_edge == 0 ||
                ((prev1 != i || prev1 == NULL) && 
                (prev2 != i || prev2 == NULL) &&
                (prev3 != i || prev3 == NULL) &&
                (prev4 != i || prev4 == NULL)))) {
            */
                found_edge = 1;
                bestEdge = weight;
                nextEdge = i;
                foundNode = 1;
            }

            if (R == 0 && (BLInstrumentationNode*)i->getTarget() == (BLInstrumentationNode*)dag->getExit()) {
                //errs() << "Found exit node with R=0!\n";
	            bestEdge = weight;
	            nextEdge = i;
                foundNode = 1;
		        break;
	        }
            /*
            else if ((R == 0 || R == 1) && bestEdge == 0)
                breakOut = 1;
                break;
            */

        }

        taken[nextEdge] = 1;

        BLInstrumentationNode* nextNode = (BLInstrumentationNode*)(nextEdge->getTarget());       

	    //errs() << "foundNode = " << foundNode << " R = " << R << " nextNode: " << nextNode << " bestEdge = " << bestEdge << "\n\n"; 	

	    if (nextNode == (BLInstrumentationNode*)dag->getExit()) 
	        break;

 	    // Terminate on the <null> 
	    //if (!nextNode->getBlock())
        //  break;
        // Move to next node
	    //prevent loops when weight = 0
        /*prev4 = prev3;
        prev3 = prev2;
	    prev2 = prev1;
	    prev1 = nextEdge;
        */
        curNode = nextNode;
        R -= bestEdge;
    }
    return path;
}
// Iterate through all possible paths in the dag
void LSTMStaticEstimatorPass::calculatePaths(BLInstrumentationDag* dag) {
  unsigned nPaths = dag->getNumberOfPaths();
  errs() << "There are " << nPaths << " paths\n";

  int stride = nPaths / MAX_PATHS;
  if (stride <= 1)
      stride = 1;

  errs() << "Using stride " << stride << "\n";

  Function* fn = dag->getRoot()->getBlock()->getParent();
  PI->setCurrentFunction(fn);
  unsigned nPathsRun = PI->pathsRun();
  if (nPathsRun == 0) {
      errs() << "This function is never run in profiling! Skipping...\n";
  }
  else {
      int n_extracted = 0;
      // Enumerate all paths in this function
      for (int i=0; i<nPaths; i++) {
          // Show progress for large values
          if (i % 10000000 == 0 && i != 0) {
              errs() << "Computed for " << i << "/" << nPaths << " paths\n";
          }

          ProfilePath* curPath = PI->getPath(i);
          unsigned n_real_count = 0;
          if (curPath) {
              n_real_count = curPath->getCount();
          }

          // We need to subsample the paths, but only if this isn't a pos example
          bool extract = false;
          if (n_real_count == 0) {
              if (i % stride == 0) {
                  extract = true;
              }
          } else {
              extract = true;
          }

    
          if (extract) {
              //compute the exact path
              std::vector<BasicBlock*> path = computePath(dag, i);

              // Extract features 
              FeatureExtractor* features = new FeatureExtractor(path);
              std::string fnName = fn->getName();
              ofs << fnName << " " << i << " "                  // Function ID
                  << n_real_count << " "                        // Ground truth
                  << path.size() << "\n"                        // Number of BB to follow
                  << features->getFeaturesLSTM();               // BBs and features
              delete features;
              n_extracted++;
          }
      }
      errs() << "Extracted " << n_extracted << " paths for this function\n\n";
  }
}

// Entry point of the module
void LSTMStaticEstimatorPass::runOnFunction(std::vector<Constant*> &ftInit,
                                 Function &F, Module &M) {
  errs() << "Running on function " << F.getName() << "\n";

  /* 
  if (F.getName().compare("_ZN11DataOutBase12write_povrayILi3ELi4EEEvRKSt6vectorINS_5PatchIXT_EXT0_EEESaIS3_EERKS1_ISsSaISsEERKNS_11PovrayFlagsERSo")) {
    errs() << "Skipping...\n";
    return;
  }
 */ 


  // Build DAG from CFG
  BLInstrumentationDag dag = BLInstrumentationDag(F);
  dag.init();

  // give each path a unique integer value
  dag.calculatePathNumbers();

  errs() << "Starting calculatePaths..." << "\n";
 
  // Calculate the features for each path 
  calculatePaths(&dag);
}

bool LSTMStaticEstimatorPass::runOnModule(Module &M) {
  errs() << "Running research module\n";

  PI = &getAnalysis<PathProfileInfo>();

  // Start outputs
  std::string fname = "feature_output.csv";
  errs() << "Writing to " << fname << "\n";
  ofs.open(fname, std::ofstream::out);

  // No main, no instrumentation!
  Function *Main = M.getFunction("main");
  if (!Main)
    Main = M.getFunction("MAIN__");

  if (!Main) {
    errs() << "WARNING: cannot run static estimation on a module"
           << " with no main function!\n";
    return false;
  }

  std::vector<Constant*> ftInit;
  unsigned functionNumber = 0;
  for (Module::iterator F = M.begin(), E = M.end(); F != E; F++) {
    if (F->isDeclaration())
      continue;

    runOnFunction(ftInit, *F, M);
  }

  ofs.close();
  return false;
}

void LSTMStaticEstimatorPass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<PathProfileInfo>();
}

// Register the path profiler as a pass
char LSTMStaticEstimatorPass::ID = 0;
static RegisterPass<LSTMStaticEstimatorPass> X("LSTMStaticEstimatorPass", "insert-lstm-static-estimation", false, false);
