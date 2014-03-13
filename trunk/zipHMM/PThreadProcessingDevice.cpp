#include "PThreadProcessingDevice.hpp"

#include "prob_spaces.hpp"
#include "hmm_utils.hpp"
#include "Stage1JobControl.hpp"
#include "Stage2JobControl.hpp"
#include "ViterbiJobControl.hpp"
#include "debug.hpp"

#include <pthread.h>
#include <map>
#include <deque>
#include <vector>

namespace zipHMM {
  
  namespace {

    // make template??

    struct InternalStage2JobControl {
      PThreadProcessingDevice &self;
      Stage2JobControl &control;
      
      InternalStage2JobControl(PThreadProcessingDevice &self, Stage2JobControl &control)
	: self(self), control(control)
      {}
    };

    struct InternalStage1JobControl {
      PThreadProcessingDevice &self;
      Stage1JobControl &control;
      
      InternalStage1JobControl(PThreadProcessingDevice &self, Stage1JobControl &control)
	: self(self), control(control)
      {}
    };

    struct ViterbiInternalJobControl {
      PThreadProcessingDevice &self;
      ViterbiJobControl &control;
      
      ViterbiInternalJobControl(PThreadProcessingDevice &self,
				ViterbiJobControl &control)
	: self(self), control(control)
      {}
    };
  }



  void *processSymbol2scaleAndSymbol2matrix(void *internalStage1JobControl_ptr) {
    InternalStage1JobControl *internalControl = static_cast<InternalStage1JobControl *>(internalStage1JobControl_ptr);
    PThreadProcessingDevice &device = internalControl->self;
    Stage1JobControl &control = internalControl->control;
    std::deque<unsigned> jobs_to_do;

    while(control.get_jobs(jobs_to_do) != 0) { // fetch jobs
      for(std::deque<unsigned>::iterator it = jobs_to_do.begin(); it != jobs_to_do.end(); ++it) { // do computations
	unsigned symbol = (*it);
	DEBUG("%d computing symbol %d\n", device.id, symbol);
	
	s_pair symbol_pair = device.getPair(symbol);
	unsigned left_symbol  = symbol_pair.second;
	unsigned right_symbol = symbol_pair.first;

	const Matrix &left_matrix  = device.getMatrix(left_symbol);
	const Matrix &right_matrix = device.getMatrix(right_symbol);
	
	Matrix::blas_mult(left_matrix, right_matrix, device.getMatrix(symbol));
	device.getScale(symbol) = std::log( device.getMatrix(symbol).normalize() ) + device.getScale(left_symbol) + device.getScale(right_symbol);
      }

      // do updates
      control.update(jobs_to_do); // now jobs_done
      jobs_to_do.clear();
    }

    return 0;
  }

  void PThreadProcessingDevice::computeSymbol2ScaleAndSymbol2Matrix(Stage1JobControl &control) {
    pthread_create(&thread, 0, processSymbol2scaleAndSymbol2matrix, new InternalStage1JobControl(*this, control));
  }


  void processLikelihood(Matrix &resultMatrix, double &resultLogLikelihood, PThreadProcessingDevice &device, size_t seqBegin, size_t seqEnd) {
    const std::vector<unsigned> &seq = device.getSeq();
            
    Matrix *result = new Matrix();
    Matrix *temp = new Matrix();

    Matrix::copy(resultMatrix, *result);
    resultLogLikelihood = LinearSpace::toLogSpace(result->normalize());
    // std::cout << "resultLogLikelihood: " << resultLogLikelihood << std::endl;

    for(size_t i = seqBegin; i < seqEnd; ++i) {
      Matrix::mult(device.getMatrix(seq[i]), *result, *temp);
      std::swap(result, temp);
      double scale = LinearSpace::toLogSpace(result->normalize());
      // std::cout << "scale: " << scale << std::endl;
      resultLogLikelihood += scale + device.getScale(seq[i]);
    }
      
    Matrix::copy(*result, resultMatrix);
      
    delete result;
    delete temp;
  }

  void *processLikelihoodVector(void *vInternalStage2JobControl) {
    InternalStage2JobControl *internalControl = static_cast<InternalStage2JobControl *>(vInternalStage2JobControl);
    PThreadProcessingDevice &device = internalControl->self;
    Stage2JobControl &control = internalControl->control;
      
    device.clearResult();

    Matrix resultMatrix;
    double resultLogLikelihood = LogSpace::ONE;
    double tempLogLikelihood = LogSpace::ONE;

    resultLogLikelihood += std::log( init_apply_em_prob(resultMatrix, device.getInitProbs(), device.getEmProbs(), device.getSeq()[0]) );

    pthread_mutex_lock(&control.mutex);
    while(control.headBlock < control.tailBlock) {
      size_t block = control.headBlock++;
      // std::cout << "Vector took block " << block << std::endl;
      pthread_mutex_unlock(&control.mutex);
	
      processLikelihood(resultMatrix, tempLogLikelihood, device, control.getBlockBegin(block), control.getBlockEnd(block));

      resultLogLikelihood += tempLogLikelihood;
	
      Matrix::copy(resultMatrix, *(control.resultMatrices[block]));
      control.resultLogLikelihoods[block] = resultLogLikelihood;
	
      pthread_mutex_lock(&control.mutex);
    }
    pthread_mutex_unlock(&control.mutex);

    delete internalControl;
    return 0;
  }

  void *processLikelihoodMatrix(void *vInternalStage2JobControl) {
    InternalStage2JobControl *internalControl = static_cast<InternalStage2JobControl *>(vInternalStage2JobControl);
    PThreadProcessingDevice &device = internalControl->self;
    Stage2JobControl &control = internalControl->control;

    device.clearResult();

    const size_t N = device.getInitProbs().get_height();

    pthread_mutex_lock(&control.mutex);
    while((control.tailBlock - control.headBlock) > N && control.tailBlock > 1) {
      size_t block = --control.tailBlock;
      // std::cout << "Matrix took block " << block << std::endl;
      pthread_mutex_unlock(&control.mutex);
	
      Matrix resultMatrix;
      double resultLogLikelihood = LogSpace::ONE;
	
      Matrix::copy( device.getMatrix(device.getSeq()[control.getBlockBegin(block)]) , resultMatrix );
      resultLogLikelihood += device.getScale( device.getSeq()[control.getBlockBegin(block)] );
      // std::cout << "resultLogLikelihood: " << resultLogLikelihood << std::endl;

      double tmpLoglikelihood = LogSpace::ONE;
      processLikelihood(resultMatrix, tmpLoglikelihood, device, control.getBlockBegin(block) + 1, control.getBlockEnd(block));
      resultLogLikelihood += tmpLoglikelihood;

      Matrix::copy(resultMatrix, *(control.resultMatrices[block]));
      control.resultLogLikelihoods[block] = resultLogLikelihood;
	
      pthread_mutex_lock(&control.mutex);
    }
    pthread_mutex_unlock(&control.mutex);

    delete internalControl;
    return 0;
  }

  void PThreadProcessingDevice::likelihoodVector(Stage2JobControl &control) {
    pthread_create(&thread, 0, processLikelihoodVector, new InternalStage2JobControl(*this, control));
  }

  void PThreadProcessingDevice::likelihoodMatrix(Stage2JobControl &control) {
    pthread_create(&thread, 0, processLikelihoodMatrix, new InternalStage2JobControl(*this, control));
  }

 
  void PThreadProcessingDevice::setParameters(const Matrix *pi, const Matrix *A, const Matrix *B, 
					      const std::map<unsigned, s_pair> *symbol2pair, double *symbol2scale, Matrix *symbol2matrix) {
    setHMM(pi, A, B);
    setSymbol2pair(symbol2pair);
    setSymbol2scale(symbol2scale);
    setSymbol2matrix(symbol2matrix);
  }

  void PThreadProcessingDevice::setHMM(const Matrix *pi, const Matrix *A, const Matrix *B) {
    this->pi = pi;
    this->A = A;
    this->B = B;
  }

  void PThreadProcessingDevice::join() {
    pthread_join(thread, 0);
  }

  void PThreadProcessingDevice::computeSymbol2logMatrix() {
    if(symbol2logMatrix == 0) {
      Matrix *symbol2logMatrix = new Matrix[B->get_width()];
      const size_t nStates = A->get_height();

      for(size_t symbol = 0; symbol < B->get_width(); ++symbol) {
	symbol2logMatrix[symbol].reset(nStates, nStates);
	for(size_t r = 0; r < nStates; ++r) {
	  for(size_t c = 0; c < nStates; ++c)
	    symbol2logMatrix[symbol](r, c) = A->at(c, r) * B->at(r, symbol);
	}
	symbol2logMatrix[symbol].log();
      }
    }
  }

  void PThreadProcessingDevice::clearCache() { // delete?
    if(symbol2logMatrix != 0)
      delete[] symbol2logMatrix;
  }

  void PThreadProcessingDevice::clearResult() { // delete?
    
  }

  void PThreadProcessingDevice::clear() { // delete?
    clearCache();
    clearResult();
  }

} // namespace zipHMM
