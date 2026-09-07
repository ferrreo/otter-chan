Vendored from tensorflow/tflite-micro `tensorflow/lite/experimental/microfrontend/lib` (Apache-2.0)
and mborgerding/kissfft (BSD-3-Clause). kiss_fft.c / tools/kiss_fftr.c are compiled only through
`kiss_fft_int16.cc` (FIXED_POINT=16 inside namespace kissfft_fixed16); `*_io.c` are build-time
tools and excluded.
