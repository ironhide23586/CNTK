//
// <copyright file="ComputationNode.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

#include "Basics.h"
#include "Matrix.h"

//#define RNN_DEBUG 1
#define DEFAULT_HIDDEN_ACTIVITY 0.1

#ifndef NOT_IMPLEMENTED
#define NOT_IMPLEMENTED \
{   \
    fprintf(stderr, "Inside File: %s  Line: %d  Function: %s  -> Feature Not Implemented.\n", __FILE__, __LINE__, __FUNCTION__); \
    throw std::logic_error("Not Implemented"); \
}
#endif

#pragma warning (disable: 4267)

//version number to control how to read and write 
#define CNTK_MODEL_VERSION_1 1
#define CURRENT_CNTK_MODEL_VERSION 1

namespace Microsoft { namespace MSR { namespace CNTK {

    enum CopyNodeFlags
    {
        copyNodeNull = 0, // invalid value
        copyNodeValue=1, // copy everything but the children links
        copyNodeChildren=2, // only copy over children links
        copyNodeAll=3, // copy everything
        copyNodeChildrenCrossNetwork=4, // allow a cross network child copy
    };

#pragma region base computation class

    template<class ElemType>
    class ComputationNode //Abstract Class that cannot be instantiated
    {
    protected:
        //std containers such as list and map does not support class reference so we need to use pointer
        typedef ComputationNode<ElemType>* ComputationNodePtr;
		typedef std::pair<ComputationNodePtr, ComputationNodePtr> ComputationArc;
        int     m_loopId;
        size_t  m_samplesInRecurrentStep; 

        /// the order in reverse graph. 
        int     m_visitedOrder;  
        int m_index;
        int m_lowlink;
        bool m_visited;
        bool m_inStack;
        int m_indexInLoop;
        vector<size_t> m_sentenceEnd;
    public:
        ComputationNode(DEVICEID_TYPE deviceId): m_functionValues(deviceId), m_gradientValues(deviceId) 
        {
            m_deviceId = deviceId;
            m_loopId = -1;
            m_samplesInRecurrentStep = 1;
            m_visitedOrder = -1;
            m_index = -1;
            m_lowlink = -1;
            m_indexInLoop = 0;
            m_visited = false;
            m_inStack = false;
        }

        virtual ~ComputationNode()
        {
#ifdef DISPLAY_DEBUG
            fprintf (stderr, "Called Destructor NodeName: %s\n",(msra::strfun::utf8 (NodeName())).c_str());
#endif
        }

        virtual const std::wstring OperationName() const = 0;
        virtual void SaveToFile(File& fstream) const
        {
            fstream << OperationName() << NodeName();
        }

        virtual void LoadFromFile(File& /*fstream*/, const size_t /*modelVersion*/, const DEVICEID_TYPE deviceId = AUTOPLACEMATRIX)
        {
            m_deviceId = deviceId;
            MoveMatricesToDevice(deviceId);
            InitRecurrentNode();
        }

        virtual void ComputeInputPartial(const size_t inputIndex) = 0;
        virtual void ComputeInputPartial(const size_t /*inputIndex*/, const size_t /*timeIdxInSeq*/) 
        {
            NOT_IMPLEMENTED;
        }
        
        virtual void EvaluateThisNode() = 0;
        // evaluate only at time index timeIdxInSeq
        virtual void EvaluateThisNode(const size_t /*timeIdxInSeq*/) 
        {
            NOT_IMPLEMENTED;
        }
        virtual void Validate() = 0;
        
        virtual void Reset() {}
        virtual void NotReset() {}

        virtual void AttachInputs(const ComputationNodePtr /*singleInput*/) 
        {
            throw std::logic_error("This operation does not support single input.");
        }

        virtual void AttachInputs(const ComputationNodePtr /*leftInput*/, const ComputationNodePtr /*rightInput*/) 
        {
            throw std::logic_error("This operation does not support two inputs.");
        }

        virtual void AttachInputs(const ComputationNodePtr /*leftInput*/, const ComputationNodePtr /*middleInput*/, const ComputationNodePtr /*rightInput*/) 
        {
            throw std::logic_error("This operation does not support three inputs.");
        }

        virtual void AttachInputs(const ComputationNodePtr /*firstInput*/, const ComputationNodePtr /*secondInput*/, const ComputationNodePtr /*thirdInput*/, const ComputationNodePtr /*fourthInput*/)
        {
            throw std::logic_error("This operation does not support four inputs.");
        }

        virtual void AttachInputs(const ComputationNodePtr /*firstInput*/, const ComputationNodePtr /*secondInput*/, const ComputationNodePtr /*thirdInput*/, 
            const ComputationNodePtr /*fourthInput*/, const ComputationNodePtr /*fifthInput*/)
        {
            throw std::logic_error("This operation does not support five inputs.");
        }

        virtual void AttachInputs(const ComputationNodePtr /*firstInput*/, const ComputationNodePtr /*secondInput*/, const ComputationNodePtr /*thirdInput*/,
            const ComputationNodePtr /*fourthInput*/, const ComputationNodePtr /*fifthInput*/, const ComputationNodePtr /* sixthInput */)
        {
            throw std::logic_error("This operation does not support six inputs.");
        }

        virtual void AttachInputs(const std::vector<ComputationNodePtr>& /*inputs*/)
        {
            throw std::logic_error("This operation does not support variable-length inputs.");
        }

        virtual void DetachInputs()
        {
            m_children.resize(0);
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            if (deviceId != AUTOPLACEMATRIX)
            {
                if (m_functionValues.GetDeviceId() != deviceId)
                {
                    bool fEmpty = m_functionValues.GetNumElements() == 0;
                    m_functionValues.TransferFromDeviceToDevice(m_functionValues.GetDeviceId(), deviceId,true, fEmpty);
                }

                if (m_gradientValues.GetDeviceId() != deviceId)
                {
                    bool fEmpty = m_gradientValues.GetNumElements() == 0;
                    m_gradientValues.TransferFromDeviceToDevice(m_gradientValues.GetDeviceId(), deviceId,true, fEmpty);
                }
            }
        }

        //making them virtual so that nodes that only copy values from it's children (e.g., dropout) can be efficient in evaluation
        virtual const Matrix<ElemType>& FunctionValues() const {return m_functionValues;}
        virtual Matrix<ElemType>& FunctionValues() { return m_functionValues;}

        //return true if the node's value should be computed before the normal training. e.g., mean and invStd of input features.
        virtual bool RequirePreCompute() const { return false;}

        virtual void DumpNodeInfo(const bool /*printValues*/, File& fstream) const
        {
            fstream << L"\n" + NodeName() + L"=" + OperationName();

            if (!IsLeaf())
            {
                fstream << wstring(L"(");
                for (size_t i=0; i<ChildrenSize(); i++)
                {
                    if (i > 0)
                        fstream << wstring(L",");
                    fstream << (Inputs(i) ? Inputs(i)->NodeName() : L"NULL");
                }
                fstream << wstring(L")");
            }
        }

        virtual void SetFunctionAndGradientSize(const int numSamples) 
        {
            size_t numRows = m_functionValues.GetNumRows();
            if (numRows > 0 && numSamples > 0)
            {
                m_functionValues.Resize(numRows, numSamples); 
                m_gradientValues.Resize(numRows, numSamples); 
            }
        }
        void ResetBound(size_t indexInBatch, size_t frameNum)
        {
            m_sentenceEnd[indexInBatch] = frameNum;
        }
        void SetLoopId(const int id)
        {
            m_loopId = id;
        }
        void SetVisitedOrder(const int id)
        {
            m_visitedOrder = id;
        }
        void SetIndex(const size_t ind)
        {
            m_index = ind;
        }

        void Setlowlink(const size_t lowlink)
        {
            m_lowlink = lowlink;
        }

        void SetVisited(const bool visited)
        {
            m_visited = visited;
        }

        void SetInStack(const bool instack)
        {
            m_inStack = instack;
        }

        void SetIndexInLoop(const size_t index)
        {
            m_indexInLoop = index;
        }

		void clearCache()
		{
			m_loopId = -1;
			m_visitedOrder = -1;
			m_index = -1;
			m_lowlink = -1;
			m_indexInLoop = 0;
			m_visited = false;
			m_inStack = false;
		}
        size_t GetIndex()
        {
            return m_index;
        }

        size_t GetVisitedOrder()
        {
            return m_visitedOrder;
        }

        size_t Getlowlink ()
        {
            return m_lowlink;
        }

        size_t GetIndexInLoop()
        {
            return m_indexInLoop;
        }

        std::wstring GetName() const
        {
            return m_nodeName;
        }

        std::vector<ComputationNodePtr>	GetChildren() const
        {
            return m_children;
        }

        bool isVisisted()
        {
            return m_visited;
        }

        bool isInStack()
        {
            return m_inStack;
        }
        int LoopId()
        {
            return m_loopId;
        }

        void SetNbrSlicesInEachRecurrentIteration(size_t bsz)
        {
            m_samplesInRecurrentStep = bsz;
            m_sentenceEnd.resize(bsz);
        }

        int64_t UpdateEvalTimeStamp()
        {
            m_evalTimeStamp = atomic_fetch_add(&s_timeStampCounter, (unsigned long long int) 1);
            return m_evalTimeStamp;
        }

        void ResetEvalTimeStamp()
        {
            m_evalTimeStamp = s_timeStampCounter;
        }

        //for debugging purpose
        virtual void PrintSelf(bool printMatrices = false) const
        {
            fprintf(stderr, "\n%ls[%lu, %lu] = %ls", NodeName().c_str(), FunctionValues().GetNumRows(),  FunctionValues().GetNumCols(), OperationName().c_str());           

            if (!IsLeaf())
            {
                fprintf(stderr, "(");           
                for (size_t i=0; i<ChildrenSize(); i++)
                {
                    if (i > 0)
                        fprintf(stderr, ", ");           
                    fprintf(stderr, "%ls[%lu, %lu]", Inputs(i)?Inputs(i)->NodeName().c_str():L"NULL", Inputs(i)->FunctionValues().GetNumRows(), Inputs(i)->FunctionValues().GetNumCols());
                }
                fprintf(stderr, ")");           
            }

            if (printMatrices)
            {
                fprintf (stderr, "\n    $$$$ Function Values\n");
                FunctionValues().Print("FunctionValue");

                fprintf (stderr, "\n    $$$$ Gradient Values\n");
                GradientValues().Print("GradientValue");
            }
        }

        const std::wstring& NodeName() const { return m_nodeName;}
        std::wstring& NodeName() { return m_nodeName;}
        
        const std::wstring& DerivativeName() const {return L"D_" + m_nodeName;}

        const Matrix<ElemType>& GradientValues() const {return m_gradientValues;}
        Matrix<ElemType>& GradientValues() {return m_gradientValues;}

        bool IsLeaf() const {return m_children.size() == 0;}
        bool& NeedGradient() {return m_needGradient;}
        const bool& NeedGradient() const {return m_needGradient; }

        void InitRecurrentNode() 
        {
            SetLoop(0);     // TODO: SetLoop() takes a bool, not an int?
        }

        bool HasLoop() const { return m_hasloop ; }
        void SetLoop(const bool bl)
        {
            m_hasloop = bl; 
        }

        virtual ComputationNodePtr FindChildInASet(const std::list<ComputationNodePtr>& loop) const
        {
            for (int i = 0; i < this->m_children.size(); i++)
            {
                if (std::find(loop.begin(), loop.end(), this->m_children[i]) != loop.end())
                {
                    return this->m_children[i];
                }
            }
            return NULL;
        }

        virtual void CopyImageSizeFromInputs()
        {
            if (!IsLeaf())
                CopyImageSizeFromInput(0); //copy from child 0 by default.
        }

        bool IsChildAnImage(const size_t index) const
        {
            if (index > ChildrenSize())
                throw invalid_argument("IsChildAnImage: out of index.");

            return (Inputs(index)->m_outputWidth != 1 || Inputs(index)->m_outputChannels != 1);
        }

        const size_t ChildrenSize() const {return m_children.size();}

        inline const ComputationNodePtr Inputs(const size_t childIndex) const 
        {
#ifdef DEBUG  // profile shows this is range check very expensive in release mode, skip it  
            if (childIndex >= m_children.size())
                throw std::invalid_argument ("childIndex is out of range.");
#endif
            return m_children[childIndex];
        }

        inline ComputationNodePtr Inputs(const size_t childIndex)
        {
#ifdef DEBUG // profile shows this is range check very expensive in release mode, skip it  
            if (childIndex >= m_children.size())
                throw std::invalid_argument ("childIndex is out of range.");
#endif
            return m_children[childIndex];
        }

        void SetInput(const size_t childIndex, const ComputationNodePtr node)
        {
            //require first nodes specified before the second to avoid null nodes condition.
           if (childIndex > m_children.size())
               throw invalid_argument("SetInput: You must specify the input for children with index less than this one first.");

           // expand the inputs to exist up to the desired index
           while (childIndex >= m_children.size())
           {
               m_children.push_back(NULL);
           }

           // set the input value
            m_children[childIndex] = node;
        }

        void ComputeGradientForChildren()
        {

            /// batch is done only for feed-forward nodes
            if (HasLoop()) 
                return;

            for (size_t i=0; i<m_children.size(); i++)
            {
                ComputationNodePtr child = m_children[i];
                if (child->NeedGradient())
                {
#ifdef DISPLAY_DEBUG
                    fprintf (stderr, "    [%lu]: %s(%s)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif              
#if DUMPOUTPUT
                    fprintf(stderr,"Backprop%d_%ls\n",i,NodeName().c_str());
#endif
                    ComputeInputPartial(i); //this computes partial wrt to the child and sums the gradient value in the child
                }
#ifdef DISPLAY_DEBUG
                else fprintf (stderr, "    [%lu]: %s(%s) (no gradient needed so don't compute for)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif              
            }
            
        }

        void ComputeGradientForChildren(const size_t timeIdxInSeq)
        {

            for (size_t i=0; i<m_children.size(); i++)
            {
                ComputationNodePtr child = m_children[i];
                if (child->NeedGradient())
                {
#ifdef DISPLAY_DEBUG
                    fprintf (stderr, "    [%lu]: %s(%s)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif              
                    ComputeInputPartial(i, timeIdxInSeq); //this computes partial wrt to the child and sums the gradient value in the child
                }
#ifdef DISPLAY_DEBUG
                else fprintf (stderr, "    [%lu]: %s(%s) (no gradient needed so don't compute for)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif              
            }
        }

        static bool IsSmaller(const ComputationNodePtr lhs, const ComputationNodePtr rhs) 
        { 
            return lhs->m_visitedOrder < rhs->m_visitedOrder;
        }

        bool IsEqualTo (const ComputationNodePtr other) const //this will be used to determine whehter two nodes are the same
        {
            if (OperationName() != other->OperationName() || m_children.size() != other->m_children.size())
                return false;

            if (NodeName() == other->NodeName())  //assume names are unique in the system
                return true;

            if (IsLeaf() && other->IsLeaf())  //since names are not equal otherwise will return above
                return false;

            for (size_t i=0; i<m_children.size(); i++)
            {
                if (!(Inputs(i) == other->Inputs(i)))
                    return false;
            }

            return true;
        }
        
        std::list<ComputationNodePtr> EnumerateNodes(const bool forwardComputation, std::vector<ComputationNodePtr>& rootOfLoop)
        {
            std::list<ComputationNodePtr> result;

            if (forwardComputation)
            {
                std::unordered_set<ComputationNodePtr> visited;
                EnumerateNodesForEval(visited, result, rootOfLoop,false);
            }
            else
            {
                result = EnumerateNodesForGradient();
            }
           
            return result;          
        }

        std::list<ComputationNodePtr> ReshuffleNodes(std::map<int, std::list<ComputationNodePtr>> recurrentResult)
        {
            std::list<ComputationNodePtr> noRecurrentResult;
            std::unordered_set<ComputationNodePtr> visited;

            ReshuffleNodesForEvalWithRecurrentLoops(visited, recurrentResult, noRecurrentResult);
           
            return noRecurrentResult;          
        }



        std::list<ComputationNodePtr> EnumerateNodes(const bool forwardComputation)
        {
            std::list<ComputationNodePtr> result;

            if (forwardComputation)
            {
                std::unordered_set<ComputationNodePtr> visited;
                EnumerateNodesForEval(visited, result);
            }
            else
            {
                result = EnumerateNodesForGradient();
            }
           
            return result;          
        }

        bool IsFuncValueOlderThanInputs() const
        {
              for (size_t i=0; i<ChildrenSize(); i++)
              {
                  //the second condition is used when the time stamp change from positive to negative
                  if (Inputs(i)->m_evalTimeStamp >= m_evalTimeStamp || Inputs(i)->m_evalTimeStamp + 1e10 < m_evalTimeStamp) 
                      return true;
              }

              return false;
        }

        void ClearGradientForChildren(const int /*iActMiniBatchSize*/)
        {
            for (size_t i=0; i<m_children.size(); i++)
            {
                ComputationNodePtr child = m_children[i];
                if (child->NeedGradient())
                {
                    if(child->GradientValues().GetMatrixType() == DENSE) 
                    {
                        child->GradientValues().Resize(child->FunctionValues().GetNumRows(), child->FunctionValues().GetNumCols());
                        child->GradientValues().SetValue(0); 
                    }
                    else
                    {
                        child->GradientValues().Reset();
                    }
                }
            }
        }

        //  [1/13/2015 erw] add to enumerate all the edges 
        void EnumerateArcs(std::unordered_set<ComputationNodePtr>& vistied, std::list<ComputationArc>& arcs)
            //  enumerate arcs that can be reached starting from the current node's children
            //  [in/out] visited record already visited nodes 
        {
            std::list<ComputationNodePtr>	tovisit;

            if (vistied.find(this) == vistied.end()) // only do when this node has not been visited before
            {
                tovisit.push_back(this);

                while (!tovisit.empty())
                {
                    ComputationNodePtr curNode = tovisit.front();
                    tovisit.pop_front();

                    if (vistied.find(curNode) == vistied.end())
                    {
                        for (size_t i = 0; i < curNode->m_children.size(); i++)
                        {
                            arcs.push_back(ComputationArc(curNode, curNode->m_children[i]));

                            if (vistied.find(curNode->m_children[i]) == vistied.end()) // this children has not been visited before 
                            {
                                tovisit.push_front(curNode->m_children[i]);		// going to visit each of the children
                            }
                        }
                        vistied.insert(curNode);
                    }
                }
            }
        }

        // NOTE: we should reimplement this to be thread-safe and use a larger than requested initialized memory block
        // we can then just wrap that memory block in a matrix of the correct dimensions since it will be const no one can change it
        // should only need one memory block per device
        static const Matrix<ElemType>& ConstOnes(const size_t rows, const size_t cols, const DEVICEID_TYPE deviceId)
        {
            if (s_constOnes.find(rows) == s_constOnes.end() ||
                s_constOnes[rows].find(cols) == s_constOnes[rows].end()) //not found
            {
                Matrix<ElemType>* matrix = new Matrix<ElemType>(rows, cols, (DEVICEID_TYPE)deviceId);
                matrix->SetValue(ElemType(1.000));
                s_constOnes[rows][cols] = matrix;
            }

            Matrix<ElemType>* m = s_constOnes[rows][cols];
            m->TransferFromDeviceToDevice(m->GetDeviceId(), deviceId);

            return *m;
        }

    protected:
        void CopyImageSizeFromInput(const size_t index, const bool outputSameAsInput = true)
        {
            if (index >= ChildrenSize())
                throw invalid_argument("CopyImageSizeFromInput: output index");
        
            ComputationNodePtr child = m_children[index];
            if (child != nullptr)
            {
                m_inputWidth = child->m_outputWidth;
                m_inputHeight = child->m_outputHeight;
                m_inputChannels = child->m_outputChannels;
            }

            if (outputSameAsInput)
            {
                m_outputWidth = m_inputWidth;
                m_outputHeight = m_inputHeight;
                m_outputChannels = m_inputChannels;
            }
        }

        virtual void PrintSelfBeforeValidation(bool allowNulls=false) const
        {
            fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());           

            if (!IsLeaf())
            {
                fprintf(stderr, "(");           
                for (size_t i=0; i<ChildrenSize(); i++)
                {
                    ComputationNodePtr child = Inputs(i);
                    if (i > 0)
                        fprintf(stderr, ", ");           

                    if (child == nullptr)
                    {
                        if (allowNulls)
                        {
                            fprintf(stderr, "NULL");
                            continue;
                        }
                        throw runtime_error("One of the children is missing.");
                    }


                    if (IsChildAnImage(i))  //image
                        fprintf(stderr, "%ls[%lu {W=%lu, H=%lu, C=%lu}, %lu]", child->NodeName().c_str(), child->FunctionValues().GetNumRows(), 
                            child->m_outputWidth, child->m_outputHeight, child->m_outputChannels, child->FunctionValues().GetNumCols());
                    else
                        fprintf(stderr, "%ls[%lu, %lu]", child->NodeName().c_str(), child->FunctionValues().GetNumRows(), child->FunctionValues().GetNumCols());

                }
                fprintf(stderr, ")");           
            }
        }

        //to be called by derived classed if that class needs to print node values
        void PrintNodeValuesToFile(const bool printValues, File& fstream) const
        {
            if (printValues)
            {
                fstream << wstring(L"\n");
                const Matrix<ElemType>&  m = FunctionValues();
                for (size_t i=0; i < m.GetNumRows(); i++)
                {
                    for (size_t j=0; j < m.GetNumCols(); j++)
                    {
                        fstream << m(i,j);
                    }
                    fstream << wstring(L"\n");
                }
                fstream << wstring(L"####################################################################");
            }
       }

        std::list<ComputationNodePtr> EnumerateNodesForGradient() 
        {
            std::list<ComputationNodePtr>  nodes = this->EnumerateNodes(true);  //get forward computation order first

            nodes.sort(IsSmaller); 
            nodes.reverse();
            
            return nodes;
        }

        std::wstring CreateUniqNodeName() const
        {
#ifdef USE_GUID_AS_NAME
            UUID uuid;
            ZeroMemory(&uuid, sizeof(UUID));
            std::wstring name;

            UuidCreate(&uuid);
            WCHAR* szUuid = nullptr;
            if (UuidToStringW(&uuid, (RPC_WSTR*)&szUuid) != RPC_S_OK)
                throw std::runtime_error("Failed to craete unique node name.");
            else
            {
              name = szUuid;
              RpcStringFreeW((RPC_WSTR*)&szUuid);
            }
#else
            int64_t id = atomic_fetch_add(&s_timeStampCounter, (unsigned long long int) 1);
            std::wstring base = L"AutoName";
            std::wstringstream sstm;
            sstm << base.c_str() << id;
            std::wstring name = sstm.str();
            //msra::strfun::wstrprintf name(L"%s%d", L"AutoName", id);
#endif

            return name;
        }

        bool ChildrenNeedGradient()  const //this is only valid when called in the forward computation order.
        {
            for (int i=0; i<m_children.size(); i++)         
            {
                if (m_children[i] == nullptr)
                    continue;
                if (m_children[i]->m_needGradient) 
                    return true;
            }
            return false;
        }

        void EnumerateNodesForEval(std::unordered_set<ComputationNodePtr>& visited, std::list<ComputationNodePtr>& result,
            std::vector<ComputationNodePtr>& sourceRecurrentNodePtr, const bool bFromDelayNode) 
        {
            if (visited.find(this) == visited.end())  //not visited
            {   
                visited.insert(this);   // have visited tagged here to avoid infinite loop over children, children's children, etc

                for (int i=0; i<m_children.size(); i++)
                {
                    if (m_children[i] == nullptr)
                        continue;
                    m_children[i]->EnumerateNodesForEval(visited, result, sourceRecurrentNodePtr, this->OperationName() == L"Delay");
                }
                
                //children first for function evaluation
                if (!IsLeaf())
                {
                    if (ChildrenNeedGradient())  //only nodes that require gradient calculation is included in gradient calculation
                        m_needGradient = true;
                    else
                        m_needGradient = false;
                }
                
                result.push_back(ComputationNodePtr(this));  //we put this in the list even if it's leaf since we need to use it to determine learnable params 
                this->m_visitedOrder = result.size();
            }
            else
            {
                if (!IsLeaf() && bFromDelayNode)
                    sourceRecurrentNodePtr.push_back(this) ;
            }
        }

        void ReshuffleNodesForEvalWithRecurrentLoops(std::unordered_set<ComputationNodePtr>& visited, std::map<int, std::list<ComputationNodePtr>>& recurrentResult, 
            std::list<ComputationNodePtr>& noRecurrentResult) 
        {
            if (visited.find(this) == visited.end())  //not visited
            {   
                visited.insert(this);   // have visited tagged here to avoid infinite loop over children, children's children, etc

                for (int i=0; i<m_children.size(); i++)
                {
                    m_children[i]->ReshuffleNodesForEvalWithRecurrentLoops(visited, recurrentResult, noRecurrentResult);
                }
                
                //children first for function evaluation
                if (!IsLeaf())
                {
                    if (ChildrenNeedGradient())  //only nodes that require gradient calculation is included in gradient calculation
                        m_needGradient = true;
                    else
                        m_needGradient = false;
                }
                
                if (LoopId() >= 0)
                {
                    recurrentResult[LoopId()].push_back(ComputationNodePtr(this));
                }
                else
                {
                    noRecurrentResult.push_back(ComputationNodePtr(this));  //we put this in the list even if it's leaf since we need to use it to determine learnable params 
                }
            }
        }

        void EnumerateNodesForEval(std::unordered_set<ComputationNodePtr>& visited, std::list<ComputationNodePtr>& result) 
        {
            if (visited.find(this) == visited.end())  //not visited
            {   
                visited.insert(this);   // have visited tagged here to avoid infinite loop over children, children's children, etc

                for (int i=0; i<m_children.size(); i++)
                {
                    m_children[i]->EnumerateNodesForEval(visited, result);
                }
                
                //children first for function evaluation
                if (!IsLeaf())
                {
                    if (ChildrenNeedGradient())  //only nodes that require gradient calculation is included in gradient calculation
                        m_needGradient = true;
                    else
                        m_needGradient = false;
                }
                
                result.push_back(ComputationNodePtr(this));  //we put this in the list even if it's leaf since we need to use it to determine learnable params 
            }
        }


    public:
        virtual void CopyTo(const ComputationNodePtr node, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            if (OperationName() != node->OperationName())
                throw std::runtime_error("Cannot copy from one node type to another node type");
            if (flags & CopyNodeFlags::copyNodeChildren)
            {
                node->m_children = m_children;
            }
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_deviceId = m_deviceId;
                node->m_needGradient = m_needGradient;
                node->m_nodeName = newName;
                node->m_evalTimeStamp = m_evalTimeStamp;

                node->m_hasloop = m_hasloop; 

                node->m_inputWidth = m_inputWidth;
                node->m_inputHeight = m_inputHeight;
                node->m_inputChannels = m_inputChannels;

                node->m_outputWidth = m_outputWidth;
                node->m_outputHeight = m_outputHeight;
                node->m_outputChannels = m_outputChannels;

                node->m_functionValues = m_functionValues; 
                node->m_gradientValues = m_gradientValues;
            }
        }

        virtual ComputationNodePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) const = 0;

    protected:

        DEVICEID_TYPE m_deviceId; //CPU=-1, >=0 GPU
        bool m_needGradient;  //only used for leaf, i.e., learnable parameters, etc.

        size_t m_inputWidth, m_inputHeight, m_inputChannels;  //how to interpret each column in the input as an image
        size_t m_outputWidth, m_outputHeight, m_outputChannels;  //how to interpret each column in the output as an image

        std::vector<ComputationNodePtr> m_children;

        std::wstring m_nodeName;
        Matrix<ElemType> m_functionValues, m_gradientValues;

        static atomic_ullong s_timeStampCounter;
        int64_t m_evalTimeStamp; //this is used to reduce unnecessary recomputation when a different node in the model is reevaluated

        static std::map<size_t, std::map<size_t, Matrix<ElemType>*>> s_constOnes;

    private:
        // for loop nodes
        bool m_hasloop; 
    };
    // add this at the start of each derived class, to get access to the members of ComputationNode
    // BUGBUG: some should be protected, not public; TODO: comment here why this is needed and how to maintain it
#define UsingComputationNodeMembers    \
        typedef ComputationNode<ElemType> B; \
protected:  \
        typedef ComputationNode<ElemType>* ComputationNodePtr;  \
public: \
        using B::AttachInputs; using B::ChildrenNeedGradient; using B::ChildrenSize; using B::ClearGradientForChildren; \
        using B::ComputeGradientForChildren; using B::ComputeInputPartial; using B::ConstOnes; using B::CopyImageSizeFromInput; \
        using B::CopyImageSizeFromInputs; using B::CopyTo; using B::CreateUniqNodeName; using B::DerivativeName; using B::DetachInputs; \
        using B::DumpNodeInfo; using B::Duplicate; using B::EnumerateNodes; using B::EnumerateNodesForEval; \
        using B::EnumerateNodesForGradient; using B::EvaluateThisNode; using B::FindChildInASet; using B::FunctionValues; \
        using B::GradientValues; using B::HasLoop; using B::InitRecurrentNode; using B::Inputs; \
        using B::IsChildAnImage; using B::IsEqualTo; using B::IsFuncValueOlderThanInputs; using B::IsLeaf; using B::IsSmaller; \
        using B::LoadFromFile; using B::MoveMatricesToDevice; using B::NeedGradient; using B::NodeName; using B::NotReset; \
        using B::OperationName; using B::PrintNodeValuesToFile; using B::PrintSelf; using B::PrintSelfBeforeValidation; \
        using B::RequirePreCompute; using B::Reset; using B::ReshuffleNodes; using B::ReshuffleNodesForEvalWithRecurrentLoops; \
        using B::SaveToFile; using B::SetFunctionAndGradientSize; using B::SetInput; using B::Validate; \
protected:  \
        using B::m_loopId; using B::m_samplesInRecurrentStep; \
        using B::m_visitedOrder; using B::m_index; using B::m_lowlink; using B::m_visited; using B::m_inStack; \
        using B::m_indexInLoop; using B::m_sentenceEnd; \
        using B::m_children; using B::m_deviceId; using B::m_evalTimeStamp; using B::m_functionValues; using B::m_gradientValues; \
        using B::m_inputChannels; using B::m_inputHeight; using B::m_inputWidth; using B::m_needGradient; using B::m_nodeName; \
        using B::m_outputChannels; using B::m_outputHeight; using B::m_outputWidth; using B::s_constOnes; using B::s_timeStampCounter

#pragma endregion base computation class

#pragma region derived operations

    template<class ElemType>
    class DropoutNode : public ComputationNode<ElemType>
    {
        UsingComputationNodeMembers;
    public:

        DropoutNode(const DEVICEID_TYPE deviceId=AUTOPLACEMATRIX, const std::wstring name = L"")  
            : ComputationNode<ElemType>(deviceId), m_maskOfDropout(deviceId)
        {
            m_nodeName = (name == L""? CreateUniqNodeName() : name);
            m_deviceId = deviceId;
            MoveMatricesToDevice(deviceId);
            m_dropoutRate = 0;
            m_randomSeed = (unsigned long)atomic_fetch_add(&s_timeStampCounter, (unsigned long long int)1);
            InitRecurrentNode();
        }

        DropoutNode(File& fstream, const size_t modelVersion, const DEVICEID_TYPE deviceId=AUTOPLACEMATRIX, const std::wstring name = L"")
            : ComputationNode<ElemType>(deviceId), m_maskOfDropout(deviceId)
        {
            m_nodeName = (name == L""? CreateUniqNodeName() : name);
            m_dropoutRate = 0;  //dropout is consisered as a training parameter and thus not reinitialized if loadfromfile
            m_randomSeed = (unsigned long)atomic_fetch_add(&s_timeStampCounter, (unsigned long long int)1);

            LoadFromFile(fstream, modelVersion, deviceId);
        }

        virtual const std::wstring OperationName() const {return TypeName();}

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex > 0)
                throw std::invalid_argument("Dropout operation only takes one input.");
            ComputeInputPartialS(m_dropoutRate, Inputs(0)->GradientValues(), m_maskOfDropout, GradientValues());
        }

        virtual void ComputeInputPartial(const size_t inputIndex, const size_t timeIdxInSeq)
        {
            if (inputIndex > 0)
                throw std::invalid_argument("Dropout operation only takes one input.");

            Matrix<ElemType> sliceInput0Grad = Inputs(0)->GradientValues().ColumnSlice(timeIdxInSeq * m_samplesInRecurrentStep, m_samplesInRecurrentStep);
            Matrix<ElemType> sliceOutputGrad = GradientValues().ColumnSlice(timeIdxInSeq * m_samplesInRecurrentStep, m_samplesInRecurrentStep);

            Matrix<ElemType> sliceMask = Matrix<ElemType>();
            if(m_dropoutRate > 0)
            {
                sliceMask = m_maskOfDropout.ColumnSlice(timeIdxInSeq * m_samplesInRecurrentStep, m_samplesInRecurrentStep);
            }

            ComputeInputPartialS(m_dropoutRate, sliceInput0Grad, sliceMask, sliceOutputGrad);
        }

        static void WINAPI ComputeInputPartialS(const ElemType dropoutRate, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& maskOfDropout, const Matrix<ElemType>& gradientValues)
        {
            if (dropoutRate > 0)
            {
                inputGradientValues.AddElementProductOf(gradientValues, maskOfDropout);
            }
            else
            {   
                inputGradientValues += gradientValues;
            }
        }

        virtual void EvaluateThisNode()  
        {
            EvaluateThisNodeS(m_dropoutRate, m_randomSeed, FunctionValues(), m_maskOfDropout, Inputs(0)->FunctionValues());
        }
        virtual void EvaluateThisNode(const size_t timeIdxInSeq) 
        {
            Matrix<ElemType> sliceInput0Value = Inputs(0)->FunctionValues().ColumnSlice(timeIdxInSeq * m_samplesInRecurrentStep, m_samplesInRecurrentStep);
			Matrix<ElemType> sliceOutputValue = Matrix <ElemType>();

            Matrix<ElemType> sliceMask = Matrix<ElemType>();
            if(m_dropoutRate > 0)
            {
                FunctionValues().Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
                m_maskOfDropout.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
                sliceMask = m_maskOfDropout.ColumnSlice(timeIdxInSeq * m_samplesInRecurrentStep, m_samplesInRecurrentStep);
            }

            sliceOutputValue = FunctionValues().ColumnSlice(timeIdxInSeq * m_samplesInRecurrentStep, m_samplesInRecurrentStep);

            EvaluateThisNodeS(m_dropoutRate, m_randomSeed, sliceOutputValue, sliceMask, sliceInput0Value);
        }

        static void WINAPI EvaluateThisNodeS(const ElemType dropoutRate, unsigned long& randomSeed, Matrix<ElemType>& functionValues, Matrix<ElemType>& maskOfDropout, const Matrix<ElemType>& inputFunctionValues)
        {
            if(dropoutRate > 0)
            {
                maskOfDropout.Resize(inputFunctionValues.GetNumRows(), inputFunctionValues.GetNumCols());

                maskOfDropout.SetUniformRandomMask(dropoutRate, ElemType(1.0) / (ElemType(1) - dropoutRate), randomSeed);
                randomSeed += 1073807359;  //1073807359 is a very large prime number to avoid collision with other dropout nodes

                functionValues.AssignElementProductOf(maskOfDropout, inputFunctionValues);
#if NANCHECK
                functionValues.HasNan("DropOut");
#endif
            }
            else
            {
                //remove this line since we can get same effect by overwritting the FunctionValues functions without copying the values
                //functionValues = inputFunctionValues;
            }
        }

        virtual const Matrix<ElemType>& FunctionValues() const 
        {
            if(m_dropoutRate > 0)
                 return m_functionValues;
            else
                return Inputs(0)->FunctionValues();
        }

        virtual Matrix<ElemType>& FunctionValues() 
        {
            if(m_dropoutRate > 0)
                 return m_functionValues;
            else
                return Inputs(0)->FunctionValues();
        }

        virtual void Validate()
        {
            PrintSelfBeforeValidation();

            if (m_children.size() != 1) 
                throw std::logic_error("Dropout operation should have one input.");

            if (Inputs(0)->FunctionValues().GetNumElements() == 0)
                throw std::logic_error("Dropout operation: the input node has 0 element.");

            FunctionValues().Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_maskOfDropout.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            CopyImageSizeFromInputs(); 
        }

        virtual void AttachInputs(const ComputationNodePtr inputNode) 
        {
            m_children.resize(1);
            m_children[0] = inputNode;
        }

        void SetDropoutRate(const ElemType val)
        {
            if (val < 0 || val >= 1) 
                throw std::logic_error("DropoutRate must be >= 0 and < 1.");
            m_dropoutRate = val;
        }

        void SetRandomSeed(const unsigned long val)
        {
            m_randomSeed = (unsigned long) val;
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            ComputationNode<ElemType>::MoveMatricesToDevice(deviceId);

            if (deviceId != AUTOPLACEMATRIX)
            {
                if (m_maskOfDropout.GetDeviceId() != deviceId)
                    m_maskOfDropout.TransferFromDeviceToDevice(m_maskOfDropout.GetDeviceId(), deviceId, true);
            }
        }

        static const std::wstring TypeName() {return L"Dropout";} 

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            ComputationNode<ElemType>::CopyTo(nodeP, newName, flags);
            DropoutNode<ElemType>* node = (DropoutNode<ElemType>*) nodeP;

            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_dropoutRate = m_dropoutRate;
                node->m_randomSeed = m_randomSeed;
                node->m_maskOfDropout = m_maskOfDropout;
            }
        }

        // copy constructor
        DropoutNode(const DropoutNode<ElemType>* node, const std::wstring& newName, const CopyNodeFlags flags)
            : ComputationNode<ElemType>(node->m_deviceId), m_maskOfDropout(node->m_deviceId)
        {
            node->CopyTo(this, newName, flags);
        }

        virtual ComputationNodePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) const
        {
            const std::wstring& name = (newName == L"")?NodeName():newName;
                
            ComputationNodePtr node = new DropoutNode<ElemType>(this, name, flags);
            return node;
        }

    private:
        ElemType m_dropoutRate;
        unsigned long m_randomSeed;

        Matrix<ElemType> m_maskOfDropout;
    };

    template class DropoutNode<float>; 
    template class DropoutNode<double>;

#pragma endregion derived operations

}}}