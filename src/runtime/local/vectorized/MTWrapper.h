/*
 * Copyright 2021 The DAPHNE Consortium
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_RUNTIME_LOCAL_VECTORIZED_MTWRAPPER_H
#define SRC_RUNTIME_LOCAL_VECTORIZED_MTWRAPPER_H

#include <runtime/local/vectorized/TaskQueues.h>
#include <runtime/local/vectorized/Tasks.h>
#include <runtime/local/vectorized/Workers.h>
#include <runtime/local/vectorized/LoadPartitioning.h>
#include <ir/daphneir/Daphne.h>

#include <thread>
#include <functional>

//TODO use the wrapper to cache threads
//TODO generalize for arbitrary inputs (not just binary)

using mlir::daphne::VectorSplit;
using mlir::daphne::VectorCombine;

template <class VT>
class MTWrapper {
private:
    uint32_t _numThreads{};

public:
    MTWrapper() : MTWrapper(std::thread::hardware_concurrency()) {}
    explicit MTWrapper(uint32_t numThreads) {
        _numThreads = (numThreads <= 0) ? 32 : numThreads;
    }
    ~MTWrapper() = default;

    // Deprecated
    [[maybe_unused]] void execute(const std::vector<std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))>>& funcs,
                 DenseMatrix<VT>*& res, DenseMatrix<VT>* input1, DenseMatrix<VT>* input2, DCTX(ctx))
    {
        execute(funcs, res, input1, input2, ctx, false);
    }

    // Deprecated
    void execute(const std::vector<std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))>>& funcs,
        DenseMatrix<VT>*& res, DenseMatrix<VT>* input1, DenseMatrix<VT>* input2, DCTX(ctx), bool verbose)
    {
        if(const char* env_m = std::getenv("DAPHNE_THREADS")){
            _numThreads= std::stoi(env_m);
        }
        // create task queue (w/o size-based blocking)
        // this the maximum possible number of tasks should we start with something smaller, and it grows as ended?!
        TaskQueue* q = new BlockingTaskQueue(input1->getNumRows());

        // create workers threads
        WorkerCPU* workers[_numThreads];
        std::thread workerThreads[_numThreads];
        for(uint32_t i=0; i<_numThreads; i++) {
            workers[i] = new WorkerCPU(q, verbose);
            workerThreads[i] = std::thread(runWorker, workers[i]);
        }

        // output allocation (currently only according to input shape only)
        if( res == nullptr )
            res = DataObjectFactory::create<DenseMatrix<VT>>(input1->getNumRows(), input1->getNumCols(), false);

        // create tasks and close input
        uint64_t rlen = input1->getNumRows();
        uint64_t startChunk=0;
        uint64_t endChunk=0;
        uint64_t batchsize = 1; // row-at-a-time
        uint64_t chunkParam=1;
        LoadPartitioning lp(STATIC, rlen, chunkParam,_numThreads, false);
        while(lp.hasNextChunk()){
            endChunk += lp.getNextChunk();
            q->enqueueTask(new SingleOpTask<VT>(
                funcs[0], res, input1, input2, startChunk, endChunk, batchsize));
            startChunk= endChunk;
        }
        q->closeInput();

        // barrier (wait for completed computation)
        for(uint32_t i=0; i<_numThreads; i++)
            workerThreads[i].join();

        // cleanups
        for(uint32_t i=0; i<_numThreads; i++)
            delete workers[i];
        delete q;
    }

    [[maybe_unused]] void execute(const std::vector<std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))>>& funcs,
                 DenseMatrix<VT> *&res, DenseMatrix<VT> **inputs, size_t numInputs, DCTX(ctx))
    {
        execute(funcs, res, inputs, numInputs, ctx, false);
    }

    void execute(const std::vector<std::function<void(DenseMatrix<VT> ***, DenseMatrix<VT> **, DCTX(ctx))>>& funcs,
                 DenseMatrix<VT> *&res,
                 DenseMatrix<VT> **inputs,
                 size_t numInputs,
                 size_t numOutputs,
                 int64_t *outRows,
                 int64_t *outCols,
                 VectorSplit *splits,
                 VectorCombine *combines, DCTX(ctx),
                 bool verbose)
    {
        if(const char* env_m = std::getenv("DAPHNE_THREADS")){
            _numThreads= std::stoi(env_m);
        }

        // create workers threads
        Worker* workers[_numThreads];
        std::thread workerThreads[_numThreads];
        auto numCUDAworkerThreads = 0ul;
        auto gpu_task_len = 0ul;
        auto len = 0ul;
        auto batchsize = 100ul; // 100-rows-at-a-time
        auto mem_required = 0ul;
        // due to possible broadcasting we have to check all inputs
        for (auto i = 0u; i < numInputs; ++i) {
            if (splits[i] == mlir::daphne::VectorSplit::ROWS) {
                len = std::max(len, inputs[i]->getNumRows());
            }
            mem_required += inputs[i]->bufferSize();
        }

        assert(numOutputs == 1 && "TODO");
        // output allocation for row-wise combine
        if(res == nullptr && outRows[0] != -1 && outCols[0] != -1) {
            auto zeroOut = combines[0] == mlir::daphne::VectorCombine::ADD;
            res = DataObjectFactory::create<DenseMatrix<VT>>(outRows[0], outCols[0], zeroOut);
        }
        mem_required += res->bufferSize();

        // lock for aggregation combine
        std::mutex resLock;

#ifdef USE_CUDA
        std::unique_ptr<TaskQueue> q_CUDA{};
        DenseMatrix<VT>* res_cuda{};
        if(ctx && ctx->useCUDA() && funcs.size() > 1/* && buffer_usage < 0.99f*/) {
            // ToDo: multi-device support :-P
            auto cctx = ctx->getCUDAContext(0);
            float taskRatioCUDA;
            auto buffer_usage = static_cast<float>(mem_required) / static_cast<float>(cctx->getMemBudget());

            // ToDo: more sophisticated method for task ratio choice
            if(buffer_usage < 1.0)
                taskRatioCUDA = 1.0;
            else
                taskRatioCUDA = 0.5;
            auto row_mem = mem_required / len;

#ifndef NDEBUG
            std::cout << "Required memory (ins/outs): " << mem_required << "\nRequired mem/row: " << row_mem
                      << "\nBuffer usage: " << buffer_usage << std::endl;
#endif
            gpu_task_len = static_cast<size_t>(std::ceil(static_cast<float>(len) * taskRatioCUDA));
//            gpu_task_len = std::floor(cctx->getMemBudget() / row_mem);
//            taskRatioCUDA = static_cast<float>(gpu_task_len) / static_cast<float>(len);
            numCUDAworkerThreads = ctx->cuda_contexts.size();
            assert(numCUDAworkerThreads == 1 && "TODO: CUDA multi-device support");
            auto blksize = static_cast<size_t>(std::floor(cctx->getMemBudget() / row_mem));
            batchsize = blksize;
#ifndef NDEBUG
        std::cout << "gpu_task_len:  " << gpu_task_len << "\ntaskRatioCUDA: " << taskRatioCUDA << "\nBlock size: "
                << blksize << std::endl;
#endif
            q_CUDA = std::make_unique<BlockingTaskQueue>(len);
            for(uint32_t i=0; i<numCUDAworkerThreads; i++) {
                workers[i] = new WorkerCPU(q_CUDA.get(), verbose);
                workerThreads[i] = std::thread(runWorker, workers[i]);
            }

            for (auto i = 0u; i < numInputs; ++i) {
                if (splits[i] == mlir::daphne::VectorSplit::ROWS) {
                    [[maybe_unused]] auto bla = static_cast<const DenseMatrix<VT>*>(inputs[i])->getValuesCUDA();
                }
            }

            res_cuda = res;
            if (combines[0] == mlir::daphne::VectorCombine::ROWS) {
                res_cuda = res->slice(0, gpu_task_len);
            }

            for (uint32_t k = 0; k < gpu_task_len; k += blksize) {
//                q_CUDA->enqueueTask(new CompiledPipelineTaskCUDA<VT>(
                q_CUDA->enqueueTask(new CompiledPipelineTask<VT>(
                        funcs[1],
                        resLock,
                        res_cuda,
                        inputs,
                        numInputs,
                        numOutputs,
                        outRows,
                        outCols,
                        splits,
                        combines,
                        k,
                        std::min(k + blksize, len),
                        batchsize, 0, ctx));
            }
            q_CUDA->closeInput();
        }
#endif


#ifndef NDEBUG
        std::cout << "spawned " << _numThreads - numCUDAworkerThreads << " CPU and " << numCUDAworkerThreads
                  << " CUDA worker threads" << std::endl;
#endif

        auto numCPPworkerThreads = 0;
        auto cpu_task_len = len - gpu_task_len;
        std::unique_ptr<TaskQueue> q{};
        DenseMatrix<VT>* res_cpp{};
        auto offset = 0ul;

        if(cpu_task_len > 0) {
#ifndef NDEBUG
            std::cout << "cpu_task_len=" << cpu_task_len << std::endl;
#endif
            numCPPworkerThreads = std::max(0ul, _numThreads - numCUDAworkerThreads);
            res_cpp = res;
            if (combines[0] == mlir::daphne::VectorCombine::ROWS && gpu_task_len > 0) {
                res_cpp = res->slice(0, cpu_task_len);
                offset = gpu_task_len;
            }

            // create task queue (w/o size-based blocking)
            q = std::make_unique<BlockingTaskQueue>(len);
            for(uint32_t i=numCUDAworkerThreads; i<_numThreads; i++) {
                workers[i] = new WorkerCPU(q.get(), verbose);
                workerThreads[i] = std::thread(runWorker, workers[i]);
            }
            // create tasks and close input
            uint64_t startChunk = gpu_task_len;
            uint64_t endChunk = 0;
            auto chunkParam = 1;
            LoadPartitioning lp(STATIC, cpu_task_len, chunkParam, numCPPworkerThreads, false);
            while (lp.hasNextChunk()) {
                endChunk += lp.getNextChunk();
                q->enqueueTask(new CompiledPipelineTask<VT>(
                        funcs[0],
                        resLock,
                        res_cpp,
                        inputs,
                        numInputs,
                        numOutputs,
                        outRows,
                        outCols,
                        splits,
                        combines,
                        startChunk,
                        endChunk,
                        batchsize, offset, ctx));
                startChunk = endChunk;
            }
            q->closeInput();
        }

        auto active_workers = numCPPworkerThreads +  numCUDAworkerThreads;
        // barrier (wait for completed computation)
        for(uint32_t i=0; i < active_workers; i++) {
            workerThreads[i].join();
        }

#ifdef USE_CUDA
        if(ctx && ctx->useCUDA() && funcs.size() > 1 && len > 1) {
            if (combines[0] == mlir::daphne::VectorCombine::ROWS) {
//                const auto& const_res_cuda = *res_cuda;
//                auto data_dest = res->getValues();
//                data_dest += res->getRowSkip() * offset;
//                CHECK_CUDART(cudaMemcpy(data_dest, const_res_cuda.getValuesCUDA(), const_res_cuda.bufferSize(), cudaMemcpyDeviceToHost));
                if(res_cuda)
                    DataObjectFactory::destroy(res_cuda);
                if(res_cpp)
                    DataObjectFactory::destroy(res_cpp);
            }
        }
#endif

        // cleanups
        for(uint32_t i=0; i < active_workers; i++)
            delete workers[i];
    }
};

#endif //SRC_RUNTIME_LOCAL_VECTORIZED_MTWRAPPER_H
