/******************************************************************************
** Copyright (c) 2015, Intel Corporation                                     **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
* ******************************************************************************/
/* Narayanan Sundaram (Intel Corp.), Michael Anderson (Intel Corp.)
 * ******************************************************************************/

#include <cstring>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <cstdlib>
#include <sys/time.h>
#include <parallel/algorithm>
#include <omp.h>
#include <cassert>

inline double sec(struct timeval start, struct timeval end)
{
    return ((double)(((end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec))))/1.0e6;
}


template<class T>
void AddFn(T a, T b, T* c, void* vsp) {
  *c = a + b ;
}


template <class V, class E=int>
class Graph {

  public:
    int nvertices;
    long long int nnz;
    int vertexpropertyowner;
    int tiles_per_dim;
    bool vertexID_randomization;


    GraphPad::SpMat<GraphPad::DCSCTile<E> > A;
    GraphPad::SpMat<GraphPad::DCSCTile<E> > AT;
    GraphPad::SpVec<GraphPad::DenseSegment<V> > vertexproperty;
    GraphPad::SpVec<GraphPad::DenseSegment<bool> > active;

  public:
    void MTXFromEdgelist(GraphPad::edgelist_t<E> A_edges);
    void getVertexEdgelist(GraphPad::edgelist_t<V> & myedges);
    void ReadMTX(const char* filename, int grid_size=1); //grid_size is deprecated
    void setAllActive();
    void setAllInactive();
    int vertexToNative(int vertex, int nsegments, int len) const;
    int nativeToVertex(int vertex, int nsegments, int len) const;
    void setActive(int v);
    void setInactive(int v);
    void setAllVertexproperty(const V& val);
    void setVertexproperty(int v, const V& val);
    V getVertexproperty(int v) const;
    bool vertexNodeOwner(const int v) const;
    void saveVertexproperty(std::string fname, bool includeHeader=true) const;
    void reset();
    void shareVertexProperty(Graph<V,E>& g);
    int getNumberOfVertices() const;
    void applyToAllVertices(void (*ApplyFn)(V, V*, void*), void* param=nullptr);
    template<class T> void applyReduceAllVertices(T* val, void (*ApplyFn)(V*, T*, void*), void (*ReduceFn)(T,T,T*,void*)=AddFn<T>, void* param=nullptr);
    ~Graph();
};



template<class V, class E>
int Graph<V,E>::vertexToNative(int vertex, int nsegments, int len) const
{
  if (vertexID_randomization) {

    int v = vertex-1;
    int npartitions = omp_get_max_threads() * 16 * nsegments;
    int height = len / npartitions;
    int vmax = height * npartitions;
    if(v >= vmax)
    {
      return v+1;
    }
    int col = v%npartitions;
    int row = v/npartitions;
    return row + col * height+ 1;
  } else {
    return vertex;
  }
}

template<class V, class E>
int Graph<V,E>::nativeToVertex(int vertex, int nsegments, int len) const
{
  if (vertexID_randomization) {
    int v = vertex-1;
    int npartitions = omp_get_max_threads() * 16 * nsegments;
    int height = len / npartitions;
    int vmax = height * npartitions;
    if(v >= vmax)
    {
      return v+1;
    }
    int col = v/height;
    int row = v%height;
    return col + row * npartitions+ 1;
  } else {
    return vertex;
  }
}

template<class V, class E>
void Graph<V,E>::MTXFromEdgelist(GraphPad::edgelist_t<E> A_edges) {

  //if (GraphPad::global_nrank == 1) {
  //  vertexID_randomization = false;
  //} else {
  vertexID_randomization = true;
  //}

  struct timeval start, end;
  gettimeofday(&start, 0);
  {
    tiles_per_dim = GraphPad::get_global_nrank();
    
    #pragma omp parallel for
    for(int i = 0 ; i < A_edges.nnz ; i++)
    {
      A_edges.edges[i].src = vertexToNative(A_edges.edges[i].src, tiles_per_dim, A_edges.m);
      A_edges.edges[i].dst = vertexToNative(A_edges.edges[i].dst, tiles_per_dim, A_edges.m);
    }

    GraphPad::AssignSpMat(A_edges, &A, tiles_per_dim, tiles_per_dim, GraphPad::partition_fn_2d);
    GraphPad::Transpose(A, &AT, tiles_per_dim, tiles_per_dim, GraphPad::partition_fn_2d);

    int m_ = A.m;
    assert(A.m == A.n);
    nnz = A.getNNZ();
      vertexproperty.AllocatePartitioned(A.m, tiles_per_dim, GraphPad::vector_partition_fn);
      V *__v = new V;
      vertexproperty.setAll(*__v);
      delete __v;
      active.AllocatePartitioned(A.m, tiles_per_dim, GraphPad::vector_partition_fn);
      active.setAll(false);

    nvertices = m_;
    vertexpropertyowner = 1;
  }
  gettimeofday(&end, 0);
  std::cout << "Finished GraphPad read + construction, time: " << sec(start,end)  << std::endl;


}

template<class V, class E>
void Graph<V,E>::ReadMTX(const char* filename, int grid_size) {
  GraphPad::edgelist_t<E> A_edges;
  GraphPad::ReadEdgesBin(&A_edges, filename, false);
  MTXFromEdgelist(A_edges);
  _mm_free(A_edges.edges);
}


template<class V, class E> 
void Graph<V,E>::setAllActive() {
  //for (int i = 0; i <= nvertices; i++) {
  //  active[i] = true;
  //}
  //memset(active, 0xff, sizeof(bool)*(nvertices));
  //GraphPad::Apply(active, &active, set_all_true);
  active.setAll(true);
}

template<class V, class E> 
void Graph<V,E>::setAllInactive() {
  //memset(active, 0x0, sizeof(bool)*(nvertices));
  //GraphPad::Apply(active, &active, set_all_false);
  active.setAll(false);
  int global_myrank = GraphPad::get_global_myrank();
  //GraphPad::Clear(&active);
  for(int segmentId = 0 ; segmentId < active.nsegments ; segmentId++)
  {
    if(active.nodeIds[segmentId] == global_myrank)
    {
      GraphPad::DenseSegment<bool>* s1 = &(active.segments[segmentId]);
      GraphPad::clear_dense_segment(s1->properties.value, s1->properties.bit_vector, s1->num_ints);
    }
  }
}

template<class V, class E> 
void Graph<V,E>::setActive(int v) {
  //active[v] = true;
  int v_new = vertexToNative(v, tiles_per_dim, nvertices);
  active.set(v_new, true);
}

template<class V, class E> 
void Graph<V,E>::setInactive(int v) {
  //active[v] = false;
  int v_new = vertexToNative(v, tiles_per_dim, nvertices);
  active.set(v_new, false);
}
template<class V, class E> 
void Graph<V,E>::reset() {
  //memset(active, 0, sizeof(bool)*(nvertices));
  setAllInactive();
  V v;
  vertexproperty.setAll(v);
  //#pragma omp parallel for num_threads(nthreads)
  //for (int i = 0; i < nvertices; i++) {
  //  V v;
  //  vertexproperty[i] = v;
  //}
}

template<class V, class E> 
void Graph<V,E>::shareVertexProperty(Graph<V,E>& g) {
  //delete [] vertexproperty;
  vertexproperty = g.vertexproperty;
  vertexpropertyowner = 0;
}

template<class V, class E> 
void Graph<V,E>::setAllVertexproperty(const V& val) {
  //#pragma omp parallel for num_threads(nthreads)
  //for (int i = 0; i < nvertices; i++) {
  //  vertexproperty[i] = val;
  //}
  vertexproperty.setAll(val);
}

template<class V, class E> 
void Graph<V,E>::setVertexproperty(int v, const V& val) {
  //vertexproperty[v] = val;
  int v_new = vertexToNative(v, tiles_per_dim, nvertices);
  vertexproperty.set(v_new, val);
}

template<class V, class E> 
void Graph<V,E>::getVertexEdgelist(GraphPad::edgelist_t<V> & myedges) {
  vertexproperty.get_edges(&myedges);
  for(unsigned int i = 0 ; i < myedges.nnz ; i++)
  {
    myedges.edges[i].src = nativeToVertex(myedges.edges[i].src, tiles_per_dim, nvertices);
  }
}

template<class V, class E> 
void Graph<V,E>::saveVertexproperty(std::string fname, bool includeHeader) const {
  GraphPad::edgelist_t<V> myedges;
  vertexproperty.get_edges(&myedges);
  for(unsigned int i = 0 ; i < myedges.nnz ; i++)
  {
    myedges.edges[i].src = nativeToVertex(myedges.edges[i].src, tiles_per_dim, nvertices);
  }
  GraphPad::SpVec<GraphPad::DenseSegment<V> > vertexproperty2;
  vertexproperty2.AllocatePartitioned(nvertices, tiles_per_dim, GraphPad::vector_partition_fn);
  vertexproperty2.ingestEdgelist(myedges);
  _mm_free(myedges.edges);
  vertexproperty2.save(fname, includeHeader);
}

template<class V, class E>
bool Graph<V,E>::vertexNodeOwner(const int v) const {
  int v_new = vertexToNative(v, tiles_per_dim, nvertices);
  return vertexproperty.node_owner(v_new);
}

template<class V, class E> 
V Graph<V,E>::getVertexproperty(const int v) const {
  //return vertexproperty[v];
  V vp ;
  int v_new = vertexToNative(v, tiles_per_dim, nvertices);
  vertexproperty.get(v_new, &vp);
  return vp;
}

template<class V, class E> 
int Graph<V,E>::getNumberOfVertices() const {
  return nvertices;
}

template<class V, class E> 
void Graph<V,E>::applyToAllVertices( void (*ApplyFn)(V, V*, void*), void* param) {
  GraphPad::Apply(vertexproperty, &vertexproperty, ApplyFn, param);
}


template<class V, class E> 
template<class T> 
void Graph<V,E>::applyReduceAllVertices(T* val, void (*ApplyFn)(V*, T*, void*), void (*ReduceFn)(T,T,T*,void*), void* param) {
  GraphPad::MapReduce(&vertexproperty, val, ApplyFn, ReduceFn, param);
}

template<class V, class E> 
Graph<V,E>::~Graph() {
  if (vertexpropertyowner) {
    //if(vertexproperty) delete [] vertexproperty;
  }
  //if (active) delete [] active;
  //if (id) delete [] id;
  //if (start_src_vertices) delete [] start_src_vertices;
  //if (end_src_vertices) delete [] end_src_vertices;

}

