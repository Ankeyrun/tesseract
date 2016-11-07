///////////////////////////////////////////////////////////////////////
// File:        network.h
// Description: Base class for neural network implementations.
// Author:      Ray Smith
// Created:     Wed May 01 16:38:06 PST 2013
//
// (C) Copyright 2013, Google Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
///////////////////////////////////////////////////////////////////////

#ifndef TESSERACT_LSTM_NETWORK_H_
#define TESSERACT_LSTM_NETWORK_H_

#include <stdio.h>
#include <cmath>

#include "genericvector.h"
#include "helpers.h"
#include "matrix.h"
#include "networkio.h"
#include "serialis.h"
#include "static_shape.h"
#include "tprintf.h"

struct Pix;
class ScrollView;
class TBOX;

namespace tesseract {

class ImageData;
class NetworkScratch;

// Enum to store the run-time type of a Network. Keep in sync with kTypeNames.
enum NetworkType {
  NT_NONE,   // The naked base class.
  NT_INPUT,  // Inputs from an image.
  // Plumbing networks combine other networks or rearrange the inputs.
  NT_CONVOLVE,     // Duplicates inputs in a sliding window neighborhood.
  NT_MAXPOOL,      // Chooses the max result from a rectangle.
  NT_PARALLEL,     // Runs networks in parallel.
  NT_REPLICATED,   // Runs identical networks in parallel.
  NT_PAR_RL_LSTM,  // Runs LTR and RTL LSTMs in parallel.
  NT_PAR_UD_LSTM,  // Runs Up and Down LSTMs in parallel.
  NT_PAR_2D_LSTM,  // Runs 4 LSTMs in parallel.
  NT_SERIES,       // Executes a sequence of layers.
  NT_RECONFIG,     // Scales the time/y size but makes the output deeper.
  NT_XREVERSED,    // Reverses the x direction of the inputs/outputs.
  NT_YREVERSED,    // Reverses the y-direction of the inputs/outputs.
  NT_XYTRANSPOSE,  // Transposes x and y (for just a single op).
  // Functional networks actually calculate stuff.
  NT_LSTM,            // Long-Short-Term-Memory block.
  NT_LSTM_SUMMARY,    // LSTM that only keeps its last output.
  NT_LOGISTIC,        // Fully connected logistic nonlinearity.
  NT_POSCLIP,         // Fully connected rect lin version of logistic.
  NT_SYMCLIP,         // Fully connected rect lin version of tanh.
  NT_TANH,            // Fully connected with tanh nonlinearity.
  NT_RELU,            // Fully connected with rectifier nonlinearity.
  NT_LINEAR,          // Fully connected with no nonlinearity.
  NT_SOFTMAX,         // Softmax uses exponential normalization, with CTC.
  NT_SOFTMAX_NO_CTC,  // Softmax uses exponential normalization, no CTC.
  // The SOFTMAX LSTMs both have an extra softmax layer on top, but inside, with
  // the outputs fed back to the input of the LSTM at the next timestep.
  // The ENCODED version binary encodes the softmax outputs, providing log2 of
  // the number of outputs as additional inputs, and the other version just
  // provides all the softmax outputs as additional inputs.
  NT_LSTM_SOFTMAX,          // 1-d LSTM with built-in fully connected softmax.
  NT_LSTM_SOFTMAX_ENCODED,  // 1-d LSTM with built-in binary encoded softmax.
  // A TensorFlow graph encapsulated as a Tesseract network.
  NT_TENSORFLOW,

  NT_COUNT  // Array size.
};

// Enum of Network behavior flags. Can in theory be set for each individual
// network element.
enum NetworkFlags {
  // Network forward/backprop behavior.
  NF_LAYER_SPECIFIC_LR = 64,  // Separate learning rate for each layer.
  NF_ADA_GRAD = 128,          // Weight-specific learning rate.
};

// Base class for network types. Not quite an abstract base class, but almost.
// Most of the time no isolated Network exists, except prior to
// deserialization.
class Network {
 public:
  Network();
  Network(NetworkType type, const STRING& name, int ni, int no);
  virtual ~Network();

  // Accessors.
  NetworkType type() const {
    return type_;
  }
  bool training() const {
    return training_;
  }
  bool needs_to_backprop() const {
    return needs_to_backprop_;
  }
  int num_weights() const { return num_weights_; }
  int NumInputs() const {
    return ni_;
  }
  int NumOutputs() const {
    return no_;
  }
  // Returns the required shape input to the network.
  virtual StaticShape InputShape() const {
    StaticShape result;
    return result;
  }
  // Returns the shape output from the network given an input shape (which may
  // be partially unknown ie zero).
  virtual StaticShape OutputShape(const StaticShape& input_shape) const {
    StaticShape result(input_shape);
    result.set_depth(no_);
    return result;
  }
  const STRING& name() const {
    return name_;
  }
  virtual STRING spec() const {
    return "?";
  }
  bool TestFlag(NetworkFlags flag) const {
    return (network_flags_ & flag) != 0;
  }

  // Initialization and administrative functions that are mostly provided
  // by Plumbing.
  // Returns true if the given type is derived from Plumbing, and thus contains
  // multiple sub-networks that can have their own learning rate.
  virtual bool IsPlumbingType() const { return false; }

  // Suspends/Enables training by setting the training_ flag. Serialize and
  // DeSerialize only operate on the run-time data if state is false.
  virtual void SetEnableTraining(bool state);

  // Sets flags that control the action of the network. See NetworkFlags enum
  // for bit values.
  virtual void SetNetworkFlags(uinT32 flags);

  // Sets up the network for training. Initializes weights using weights of
  // scale `range` picked according to the random number generator `randomizer`.
  // Note that randomizer is a borrowed pointer that should outlive the network
  // and should not be deleted by any of the networks.
  // Returns the number of weights initialized.
  virtual int InitWeights(float range, TRand* randomizer);

  // Converts a float network to an int network.
  virtual void ConvertToInt() {}

  // Provides a pointer to a TRand for any networks that care to use it.
  // Note that randomizer is a borrowed pointer that should outlive the network
  // and should not be deleted by any of the networks.
  virtual void SetRandomizer(TRand* randomizer);

  // Sets needs_to_backprop_ to needs_backprop and returns true if
  // needs_backprop || any weights in this network so the next layer forward
  // can be told to produce backprop for this layer if needed.
  virtual bool SetupNeedsBackprop(bool needs_backprop);

  // Returns the most recent reduction factor that the network applied to the
  // time sequence. Assumes that any 2-d is already eliminated. Used for
  // scaling bounding boxes of truth data and calculating result bounding boxes.
  // WARNING: if GlobalMinimax is used to vary the scale, this will return
  // the last used scale factor. Call it before any forward, and it will return
  // the minimum scale factor of the paths through the GlobalMinimax.
  virtual int XScaleFactor() const {
    return 1;
  }

  // Provides the (minimum) x scale factor to the network (of interest only to
  // input units) so they can determine how to scale bounding boxes.
  virtual void CacheXScaleFactor(int factor) {}

  // Provides debug output on the weights.
  virtual void DebugWeights() {
    tprintf("Must override Network::DebugWeights for type %d\n", type_);
  }

  // Writes to the given file. Returns false in case of error.
  // Should be overridden by subclasses, but called by their Serialize.
  virtual bool Serialize(TFile* fp) const;
  // Reads from the given file. Returns false in case of error.
  // If swap is true, assumes a big/little-endian swap is needed.
  // Should be overridden by subclasses, but NOT called by their DeSerialize.
  virtual bool DeSerialize(bool swap, TFile* fp);

  // Updates the weights using the given learning rate and momentum.
  // num_samples is the quotient to be used in the adagrad computation iff
  // use_ada_grad_ is true.
  virtual void Update(float learning_rate, float momentum, int num_samples) {}
  // Sums the products of weight updates in *this and other, splitting into
  // positive (same direction) in *same and negative (different direction) in
  // *changed.
  virtual void CountAlternators(const Network& other, double* same,
                                double* changed) const {}

  // Reads from the given file. Returns NULL in case of error.
  // If swap is true, assumes a big/little-endian swap is needed.
  // Determines the type of the serialized class and calls its DeSerialize
  // on a new object of the appropriate type, which is returned.
  static Network* CreateFromFile(bool swap, TFile* fp);

  // Runs forward propagation of activations on the input line.
  // Note that input and output are both 2-d arrays.
  // The 1st index is the time element. In a 1-d network, it might be the pixel
  // position on the textline. In a 2-d network, the linearization is defined
  // by the stride_map. (See networkio.h).
  // The 2nd index of input is the network inputs/outputs, and the dimension
  // of the input must match NumInputs() of this network.
  // The output array will be resized as needed so that its 1st dimension is
  // always equal to the number of output values, and its second dimension is
  // always NumOutputs(). Note that all this detail is encapsulated away inside
  // NetworkIO, as are the internals of the scratch memory space used by the
  // network. See networkscratch.h for that.
  // If input_transpose is not NULL, then it contains the transpose of input,
  // and the caller guarantees that it will still be valid on the next call to
  // backward. The callee is therefore at liberty to save the pointer and
  // reference it on a call to backward. This is a bit ugly, but it makes it
  // possible for a replicating parallel to calculate the input transpose once
  // instead of all the replicated networks having to do it.
  virtual void Forward(bool debug, const NetworkIO& input,
                       const TransposedArray* input_transpose,
                       NetworkScratch* scratch, NetworkIO* output) {
    tprintf("Must override Network::Forward for type %d\n", type_);
  }

  // Runs backward propagation of errors on fwdX_deltas.
  // Note that fwd_deltas and back_deltas are both 2-d arrays as with Forward.
  // Returns false if back_deltas was not set, due to there being no point in
  // propagating further backwards. Thus most complete networks will always
  // return false from Backward!
  virtual bool Backward(bool debug, const NetworkIO& fwd_deltas,
                        NetworkScratch* scratch,
                        NetworkIO* back_deltas) {
    tprintf("Must override Network::Backward for type %d\n", type_);
    return false;
  }

  // === Debug image display methods. ===
  // Displays the image of the matrix to the forward window.
  void DisplayForward(const NetworkIO& matrix);
  // Displays the image of the matrix to the backward window.
  void DisplayBackward(const NetworkIO& matrix);

  // Creates the window if needed, otherwise clears it.
  static void ClearWindow(bool tess_coords, const char* window_name,
                          int width, int height, ScrollView** window);

  // Displays the pix in the given window. and returns the height of the pix.
  // The pix is pixDestroyed.
  static int DisplayImage(Pix* pix, ScrollView* window);

 protected:
  // Returns a random number in [-range, range].
  double Random(double range);

 protected:
  NetworkType type_;          // Type of the derived network class.
  bool training_;             // Are we currently training?
  bool needs_to_backprop_;    // This network needs to output back_deltas.
  inT32 network_flags_;       // Behavior control flags in NetworkFlags.
  inT32 ni_;                  // Number of input values.
  inT32 no_;                  // Number of output values.
  inT32 num_weights_;         // Number of weights in this and sub-network.
  STRING name_;               // A unique name for this layer.

  // NOT-serialized debug data.
  ScrollView* forward_win_;   // Recognition debug display window.
  ScrollView* backward_win_;  // Training debug display window.
  TRand* randomizer_;         // Random number generator.

  // Static serialized name/type_ mapping. Keep in sync with NetworkType.
  static char const* const kTypeNames[NT_COUNT];
};


}  // namespace tesseract.

#endif  // TESSERACT_LSTM_NETWORK_H_
