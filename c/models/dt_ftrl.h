//------------------------------------------------------------------------------
// Copyright 2018 H2O.ai
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//------------------------------------------------------------------------------
#ifndef dt_MODELS_FTRL_h
#define dt_MODELS_FTRL_h
#include "py_datatable.h"
#include "utils/parallel.h"
#include "models/hash.h"
#include "models/utils.h"


using hashptr = std::unique_ptr<Hash>;

namespace dt {

template <typename T> 
struct FtrlParams {
    T alpha;
    T beta;
    T lambda1;
    T lambda2;
    uint64_t nbins;
    size_t nepochs;
    bool interactions;
    size_t : 56;
};


template <typename T>
class Ftrl {
  private:
    // Model datatable and column data pointers
    dtptr dt_model;
    T* z;
    T* n;

    // Feature importance datatable and a column data pointer
    dtptr dt_fi;
    T* fi;

    // FTRL fitting parameters
    FtrlParams<T> params;

    // Number of columns in a fitting datatable and a total number of features
    size_t ncols;
    size_t nfeatures;

    // Whether or not the FTRL model was trained, `false` by default.
    // `fit(...)` and `set_model(...)` methods will set this to `true`.
    bool model_trained;
    size_t : 56;

    // Column hashers and a vector of hashed column names
    std::vector<hashptr> hashers;
    std::vector<uint64_t> colnames_hashes;

  public:
    Ftrl(FtrlParams<T>);

    static const std::vector<std::string> model_colnames;
    static const FtrlParams<T> default_params;

    // Fitting and predicting methods
    template <typename U, typename F>
    void fit(const DataTable*, const Column*, F);
    template<typename F> dtptr predict(const DataTable*, F f);
    double predict_row(const uint64ptr&, tptr<T>&);
    void update(const uint64ptr&, tptr<T>&, T, T);

    // Model and feature importance handling methods
    void create_model();
    void reset_model();
    void init_weights();
    void create_fi();
    void reset_fi();
    void init_fi();
    void define_features(size_t);

    // Hashing methods
    void create_hashers(const DataTable*);
    static hashptr create_colhasher(const Column*);
    void hash_row(uint64ptr&, size_t);

    // Model validation methods
    static bool is_dt_valid(const dtptr&t, size_t, size_t);
    bool is_trained();

    // Getters
    DataTable* get_model();
    DataTable* get_fi();
    size_t get_nfeatures();
    size_t get_ncols();
    std::vector<uint64_t> get_colnames_hashes();
    T get_alpha();
    T get_beta();
    T get_lambda1();
    T get_lambda2();
    uint64_t get_nbins();
    size_t get_nepochs();
    bool get_interactions();
    FtrlParams<T> get_params();

    // Setters
    void set_model(DataTable*);
    void set_fi(DataTable*);
    void set_alpha(T);
    void set_beta(T);
    void set_lambda1(T);
    void set_lambda2(T);
    void set_nbins(uint64_t);
    void set_nepochs(size_t);
    void set_interactions(bool);
};


/*
*  Fit FTRL model on a datatable.
*/
template <typename T>
template <typename U, typename F>
void Ftrl<T>::fit(const DataTable* dt_X, const Column* c_y, F f) {
  define_features(dt_X->ncols);
  is_dt_valid(dt_model, params.nbins, 2)? init_weights() : create_model();
  is_dt_valid(dt_fi, nfeatures, 1)? init_fi() : create_fi();

  // Create column hashers
  create_hashers(dt_X);

  // Get the target column
  auto d_y = static_cast<const U*>(c_y->data());
  const RowIndex ri_y = c_y->rowindex();

  // Do training for `nepochs`.
  for (size_t e = 0; e < params.nepochs; ++e) {
    #pragma omp parallel num_threads(config::nthreads)
    {
      // Arrays to store hashed features and `w` weights.
      uint64ptr x = uint64ptr(new uint64_t[nfeatures]);
      tptr<T> w = tptr<T>(new T[nfeatures]);

      size_t ith = static_cast<size_t>(omp_get_thread_num());
      size_t nth = static_cast<size_t>(omp_get_num_threads());

      for (size_t i = ith; i < dt_X->nrows; i += nth) {
          size_t j = ri_y[i];
          if (j != RowIndex::NA && !ISNA<U>(d_y[j])) {
            hash_row(x, i);
            T p = f(predict_row(x, w));
            T y = static_cast<T>(d_y[j]);
            update(x, w, p, y);
          }
      }
    }
  }
  model_trained = true;
}


/*
*  Make predictions for a test datatable and return targets as a new datatable.
*/
template <typename T>
template<typename F>
dtptr Ftrl<T>::predict(const DataTable* dt_X, F f) {
  xassert(model_trained);
  define_features(dt_X->ncols);
  init_weights();
  is_dt_valid(dt_fi, nfeatures, 1)? init_fi() : create_fi();

  // Re-create hashers as `stype`s for prediction may be different from
  // those used for fitting
  create_hashers(dt_X);

  // Create a datatable to store targets
  dtptr dt_y;
  Column* col_target = Column::new_data_column(stype<T>::get_stype(), dt_X->nrows);
  dt_y = dtptr(new DataTable({col_target}, {"target"}));
  auto d_y = static_cast<double*>(dt_y->columns[0]->data_w());

  #pragma omp parallel num_threads(config::nthreads)
  {
    uint64ptr x = uint64ptr(new uint64_t[nfeatures]);
    tptr<T> w = tptr<T>(new T[nfeatures]);

    size_t ith = static_cast<size_t>(omp_get_thread_num());
    size_t nth = static_cast<size_t>(omp_get_num_threads());

    for (size_t i = ith; i < dt_X->nrows; i += nth) {
      hash_row(x, i);
      d_y[i] = f(predict_row(x, w));
    }
  }
  return dt_y;
}


} // namespace dt

#endif
