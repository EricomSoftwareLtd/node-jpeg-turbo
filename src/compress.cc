#include "exports.h"
using namespace Napi;

static char errStr[NJT_MSG_LENGTH_MAX] = "No error";
#define _throw(m) {snprintf(errStr, NJT_MSG_LENGTH_MAX, "%s", m); retval=-1; goto bailout;}

void compressBufferFreeCallback(Env env, char *data) {
  tjFree((unsigned char*) data);
}

int compress(unsigned char* srcData, uint32_t format, uint32_t width, uint32_t stride, uint32_t height, uint32_t jpegSubsamp, int quality, int bpp, unsigned long* jpegSize, unsigned char** dstData, uint32_t dstBufferLength) {
  int retval = 0;
  int err;

  tjhandle handle = NULL;
  int flags = TJFLAG_FASTDCT;
  uint32_t dstLength = 0;

  switch (jpegSubsamp) {
    case SAMP_444:
    case SAMP_422:
    case SAMP_420:
    case SAMP_GRAY:
    case SAMP_440:
      break;
    default:
      _throw("Invalid subsampling method");
  }

  // Set up buffers if required
  dstLength = tjBufSize(width, height, jpegSubsamp);
  if (dstBufferLength > 0) {
    if (dstLength > dstBufferLength) {
      _throw("Pontentially insufficient output buffer");
    }
    flags |= TJFLAG_NOREALLOC;
  }

  handle = tjInitCompress();
  if (handle == NULL) {
    _throw(tjGetErrorStr());
  }

  err = tjCompress2(handle, srcData, width, stride * bpp, height, format, dstData, jpegSize, jpegSubsamp, quality, flags);

  if (err != 0) {
    _throw(tjGetErrorStr());
  }

  bailout:
  if (handle != NULL) {
    err = 0;
    err = tjDestroy(handle);
    // If we already have an error retval wont be 0 so in that case we don't want to overwrite error message
    // Also cant use _throw here because infinite-loop
    if (err != 0 && retval == 0) {
      snprintf(errStr, NJT_MSG_LENGTH_MAX, "%s", tjGetErrorStr());
    }
  }

  return retval;
}

class CompressWorker : public AsyncWorker {
  public:
    CompressWorker(Function &callback, unsigned char* srcData, uint32_t format, uint32_t width, uint32_t stride, uint32_t height, uint32_t jpegSubsamp, int quality, int bpp, Object &dstObject, unsigned char* dstData, uint32_t dstBufferLength) :
      AsyncWorker(callback),
      srcData(srcData),
      format(format),
      width(width),
      stride(stride),
      height(height),
      jpegSubsamp(jpegSubsamp),
      quality(quality),
      bpp(bpp),
      jpegSize(0),
      dstData(dstData),
      dstBufferLength(dstBufferLength) {
        if (dstBufferLength > 0) {
          this->dstObject = Persistent(dstObject);
        }
      }
    ~CompressWorker() {}

    void Execute () override {
      int err;

      err = compress(
          this->srcData,
          this->format,
          this->width,
          this->stride,
          this->height,
          this->jpegSubsamp,
          this->quality,
          this->bpp,
          &this->jpegSize,
          &this->dstData,
          this->dstBufferLength);

      if(err != 0) {
        SetError(errStr);
      }
    }

    void OnOK () override {
      Napi::Env env = Callback().Env();

      Object obj = Object::New(env);
      Object dstObject;

      if (this->dstBufferLength > 0) {
        dstObject = this->dstObject.Value();
      }
      else {
        dstObject = Buffer<char>::New(env, (char*)this->dstData, this->jpegSize, compressBufferFreeCallback);
      }

      obj.Set("data", dstObject);
      obj.Set("size", this->jpegSize);

      Callback().Call({ env.Null(), obj });
    }

  private:
    unsigned char* srcData;
    uint32_t format;
    uint32_t width;
    uint32_t stride;
    uint32_t height;
    uint32_t jpegSubsamp;
    int quality;
    int bpp;
    unsigned long jpegSize;
    ObjectReference dstObject;
    unsigned char* dstData;
    uint32_t dstBufferLength;
};

Value compressParse(const CallbackInfo& info, bool async) {
  Env env = info.Env();

  int retval = 0;
  int cursor = 0;

  // Input
  Function callback;
  Object srcObject;
  uint32_t srcBufferLength = 0;
  unsigned char* srcData = NULL;
  Object dstObject;
  uint32_t dstBufferLength = 0;
  unsigned char* dstData = NULL;
  Object options;
  Value formatValue;
  uint32_t format = 0;
  Value sampValue;
  uint32_t jpegSubsamp = NJT_DEFAULT_SUBSAMPLING;
  Value widthValue;
  uint32_t width = 0;
  Value heightValue;
  uint32_t height = 0;
  Value strideValue;
  uint32_t stride;
  Value qualityValue;
  int quality = NJT_DEFAULT_QUALITY;
  int bpp = 0;

  // Output
  unsigned long jpegSize = 0;

  // Try to find callback here, so if we want to throw something we can use callback's err
  if (async) {
    if (info[info.Length() - 1].IsFunction()) {
      callback = info[info.Length() - 1].As<Function>();
    }
    else {
      _throw("Missing callback");
    }
  }

  if ((async && info.Length() < 3) || (!async && info.Length() < 2)) {
    _throw("Too few arguments");
  }

  // Input buffer
  srcObject = info[cursor++].As<Object>();
  if (!srcObject.IsBuffer()) {
    _throw("Invalid source buffer");
  }
  srcBufferLength = (uint32_t) srcObject.As<Buffer<char>>().Length();
  srcData = (unsigned char*) srcObject.As<Buffer<char>>().Data();

  // Options
  options = info[cursor++].As<Object>();

  // Check if options we just got is actually the destination buffer
  // If it is, pull new object from info and set that as options
  if (options.IsBuffer() && info.Length() > cursor) {
    dstObject = options;
    options = info[cursor++].As<Object>();
    dstBufferLength = (uint32_t) dstObject.As<Buffer<char>>().Length();
    dstData = (unsigned char*) dstObject.As<Buffer<char>>().Data();
  }

  if (!options.IsObject()) {
    _throw("Options must be an object");
  }

  // Format of input buffer
  formatValue = options.Get("format");
  if (formatValue.IsEmpty() || formatValue.IsUndefined()) {
    _throw("Missing format");
  }
  if (!isNapiInt(env, formatValue))
  {
    _throw("Invalid input format");
  }
  format = formatValue.ToNumber();

  // Subsampling
  sampValue = options.Get("subsampling");
  if (!sampValue.IsEmpty() && !sampValue.IsUndefined())
  {
    if (!isNapiInt(env, sampValue))
    {
      _throw("Invalid subsampling method");
    }
    jpegSubsamp = sampValue.ToNumber();
  }

  // Width
  widthValue = options.Get("width");
  if (widthValue.IsEmpty() || widthValue.IsUndefined())
  {
    _throw("Missing width");
  }
  if (!isNapiInt(env, widthValue))
  {
    _throw("Invalid width value");
  }
  width = widthValue.ToNumber();

  // Height
  heightValue = options.Get("height");
  if (heightValue.IsEmpty() || heightValue.IsUndefined()) {
    _throw("Missing height");
  }
  if (!isNapiInt(env, heightValue))
  {
    _throw("Invalid height value");
  }
  height = heightValue.ToNumber();

  // Stride
  strideValue = options.Get("stride");
  if (!strideValue.IsEmpty() && !strideValue.IsUndefined())
  {
    if (!isNapiInt(env, strideValue))
    {
      _throw("Invalid stride value");
    }
    stride = strideValue.ToNumber();
  }
  else {
    stride = width;
  }

  // Quality
  qualityValue = options.Get("quality");
  if (!qualityValue.IsEmpty() && !qualityValue.IsUndefined())
  {
    if (!isNapiInt(env, qualityValue) || qualityValue.ToNumber().Int32Value() > 100u)
    {
      _throw("Invalid quality value");
    }
    quality = qualityValue.ToNumber();
  }

  // Figure out bpp from format (needed to calculate output buffer size)
  switch (format) {
    case FORMAT_GRAY:
      bpp = 1;
      break;
    case FORMAT_RGB:
    case FORMAT_BGR:
      bpp = 3;
      break;
    case FORMAT_RGBX:
    case FORMAT_BGRX:
    case FORMAT_XRGB:
    case FORMAT_XBGR:
    case FORMAT_RGBA:
    case FORMAT_BGRA:
    case FORMAT_ABGR:
    case FORMAT_ARGB:
      bpp = 4;
      break;
    default:
      _throw("Invalid input format");
  }

  if (srcBufferLength < stride * height * bpp) {
    _throw("Source data is not long enough");
  }

  // Do either async or sync compress
  if (async) {
    CompressWorker* worker = new CompressWorker(callback, srcData, format, width, stride, height, jpegSubsamp, quality, bpp, dstObject, dstData, dstBufferLength);
    worker->Queue();
    return env.Null();
  }
  else {
    retval = compress(
        srcData,
        format,
        width,
        stride,
        height,
        jpegSubsamp,
        quality,
        bpp,
        &jpegSize,
        &dstData,
        dstBufferLength);

    if(retval != 0) {
      // Compress will set the errStr
      goto bailout;
    }
    Object obj = Object::New(env);
    if (dstBufferLength == 0) {
      dstObject = Buffer<char>::New(env, (char*)dstData, jpegSize, compressBufferFreeCallback);
    }

    obj.Set("data", dstObject);
    obj.Set("size", jpegSize);
    return obj;
  }

  // If we have error throw error or call callback with error
  bailout:
  if (retval != 0) {
    TypeError::New(env, errStr).ThrowAsJavaScriptException();
  }

  return env.Null();
}

Value DiffArea(const CallbackInfo& info) {
  Env env = info.Env();
  int retval = 0;
  
  Object buffer0;
  uint32_t* img0 = NULL;

  Object buffer1;
  uint32_t* img1 = NULL;

  Object diffBuffer;

  Object options;
  Value widthValue;
  uint32_t width = 0;
  Value heightValue;
  uint32_t height = 0;


  
  int x0 = UINT16_MAX,
      y0 = UINT16_MAX,
      x1 = 0, y1 = 0;


  Object result;
  int rectWidth, rectHeight;
  Object rect;

  uint32_t dstBufferLength = 0;
  unsigned char* dstData = NULL;

  // Parse arguments

  if(info.Length() < 4) {
    _throw("Too few arguments")
  }

  buffer0 = info[0].As<Object>();
  if (!buffer0.IsBuffer()) {
    _throw("Invalid first buffer");
  }
  img0 = (uint32_t*) buffer0.As<Buffer<uint32_t>>().Data();

  buffer1 = info[1].As<Object>();
  if (!buffer1.IsBuffer()) {
    _throw("Invalid second buffer");
  }
  img1 = (uint32_t*) buffer1.As<Buffer<uint32_t>>().Data();

  diffBuffer = info[2].As<Object>();
  if (!diffBuffer.IsBuffer()) {
    _throw("Invalid diff buffer");
  }
  dstData = (unsigned char*) diffBuffer.As<Buffer<unsigned char>>().Data();

  options = info[3].As<Object>();

  if (!options.IsObject()) {
    _throw("Image size must be an object");
  }

    // Width
  widthValue = options.Get("width");
  if (widthValue.IsEmpty() || widthValue.IsUndefined())
  {
    _throw("Missing width");
  }
  if (!isNapiInt(env, widthValue))
  {
    _throw("Invalid width value");
  }
  width = widthValue.ToNumber();

  // Height
  heightValue = options.Get("height");
  if (heightValue.IsEmpty() || heightValue.IsUndefined()) {
    _throw("Missing height");
  }
  if (!isNapiInt(env, heightValue))
  {
    _throw("Invalid height value");
  }
  height = heightValue.ToNumber();



  // Get different Rectangle
            
  for (int yc = 0; yc < height; ++yc) {
      for (int xc = 0; xc < width; ++xc) {
          int i = yc * width + xc;
          if (img0[i] != img1[i]) {
              x0 = std::min(x0, xc); // top left corner
              y0 = std::min(y0, yc);

              x1 = std::max(x1, xc); // bottom right corner
              y1 = std::max(y1, yc);

              xc = x1; // we know this line is dirty, so skip to the end of the current dirty rect
          }
      }
  }


  if (x0 != UINT16_MAX) { // images not equal
    rectWidth = x1 - x0 + 1;
    rectHeight = y1 - y0 + 1;

    result = Object::New(env);
    rect = Object::New(env);
    rect.Set("x", x0);
    rect.Set("y", y0);
    rect.Set("width", rectWidth);
    rect.Set("height", rectHeight);
    
    dstBufferLength = rectWidth * rectHeight * 4;
    // manual crop copy line by line
    for (int yc = y0; yc <= y1; yc++) {
      memcpy(dstData + (yc - y0) * rectWidth * 4, ((unsigned char*)img1 ) + (yc * width + x0) * 4, rectWidth * 4);
    }

    result.Set("rect", rect);
    result.Set("size", dstBufferLength); // buffer will be sliced in index.js from the preallocated diff buffer
    return result;
  } 

  bailout:
  if (retval != 0) {
    TypeError::New(env, errStr).ThrowAsJavaScriptException();
  }

  return env.Null();
}

Value CompressSync(const CallbackInfo& info) {
  return compressParse(info, false);
}

void Compress(const CallbackInfo& info) {
  compressParse(info, true);
}

