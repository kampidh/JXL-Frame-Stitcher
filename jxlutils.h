#ifndef JXLUTILS_H
#define JXLUTILS_H

#include <utility>

#include <QDebug>
#include <QFile>
#include <QString>
#include <QImage>

#include <jxl/encode_cxx.h>

enum EncodeBitDepth {
    ENC_BIT_8 = 0,
    ENC_BIT_16,
    ENC_BIT_16F,
    ENC_BIT_32F
};

enum EncodeColorSpace {
    ENC_CS_SRGB = 0,
    ENC_CS_SRGB_LINEAR,
    ENC_CS_P3,
    ENC_CS_INHERIT_FIRST,
    ENC_CS_RAW
};

// callback taken from https://github.com/libjxl/libjxl/blob/main/lib/jxl/base/c_callback_support.h
// honestly I'm not even sure what's happening here yet.. hehe
namespace jxfrstch
{
template<typename T>
struct MethodToCCallbackHelper {
};

template<typename T, typename R, typename... Args>
struct MethodToCCallbackHelper<R (T::*)(Args...)> {
    template<R (T::*method)(Args...)>
    static R Call(void *opaque, Args... args)
    {
        return (reinterpret_cast<T *>(opaque)->*method)(std::forward<Args>(args)...);
    }
};

#define METHOD_TO_C_CALLBACK(method) jxfrstch::MethodToCCallbackHelper<decltype(method)>::Call<method>

// this one referenced from https://github.com/libjxl/libjxl/blob/main/tools/cjxl_main.cc
// refitting for Qt file handling and bytearray
struct JxlOutputProcessor {
    bool SetOutputPath(QString pat)
    {
        outFile.setFileName(pat);
        outFile.open(QIODevice::WriteOnly);
        if (!outFile.isWritable()) {
            outFile.close();
            return false;
        }
        return true;
    }

    void CloseOutputFile()
    {
        outFile.close();
    }

    void DeleteOutputFile()
    {
        outFile.remove();
    }

    JxlEncoderOutputProcessor GetOutputProcessor()
    {
        return JxlEncoderOutputProcessor{this,
                                         METHOD_TO_C_CALLBACK(&JxlOutputProcessor::GetBuffer),
                                         METHOD_TO_C_CALLBACK(&JxlOutputProcessor::ReleaseBuffer),
                                         METHOD_TO_C_CALLBACK(&JxlOutputProcessor::Seek),
                                         METHOD_TO_C_CALLBACK(&JxlOutputProcessor::SetFinalizedPosition)};
    }

    void *GetBuffer(size_t *size)
    {
        *size = std::min<size_t>(*size, 1u << 16);
        if (output.size() < *size) {
            output.resize(*size);
        }
        return output.data();
    }

    void ReleaseBuffer(size_t written_bytes)
    {
        if (outFile.isOpen()) {
            if (outFile.write(reinterpret_cast<const char *>(output.data()), written_bytes) != written_bytes) {
                qWarning() << "Failed to write" << written_bytes << "bytes to output";
            }
        } else {
            qWarning() << "ReleaseBuffer failed, file not open";
        }
        output.clear();
    }

    void Seek(uint64_t position)
    {
        if (outFile.isOpen()) {
            outFile.seek(position);
        } else {
            qWarning() << "Seek failed, file not open";
        }
    }

    void SetFinalizedPosition(uint64_t finalized_position)
    {
        this->finalized_position = finalized_position;
    }

    QFile outFile;
    QByteArray output;
    size_t finalized_position = 0;
};

struct InputFileData {
    bool isRefFrame{false};
    uint16_t frameDuration{1};
    uint8_t frameReference{0};
    int16_t frameXPos{0};
    int16_t frameYPos{0};
    JxlBlendMode blendMode{JXL_BLEND_BLEND};
    QString filename{};
    QString frameName{};

    // lexical comparison
    bool operator<(const InputFileData &rhs) const
    {
        return this->filename.compare(rhs.filename) < 0;
    }

    bool operator==(const QString &rhs) const
    {
        return this->filename == rhs;
    }

    bool operator==(const InputFileData &rhs) const
    {
        return this->filename == rhs.filename;
    }
};

struct EncodeParams {
    double distance{0.0};
    double frameTimeMs{0.0};

    int effort{1};
    int numerator{1};
    int denominator{1};
    int loops{0};

    EncodeColorSpace colorSpace{ENC_CS_SRGB};
    EncodeBitDepth bitDepth{ENC_BIT_8};

    bool animation{true};
    bool alpha{true};
    bool losslessAlpha{true};
    bool premulAlpha{false};
    bool lossyModular{false};

    QString outputFileName{};
};

template<typename T>
void QImageToBuffer(const QImage &img, QByteArray &ba, size_t pxsize, bool alpha)
{
    auto srcPointer = reinterpret_cast<const T *>(img.constBits());
    auto dstPointer = reinterpret_cast<T *>(ba.data());
    const int chan = (alpha) ? 4 : 3;
    for (size_t i = 0; i < pxsize; i++) {
        memcpy(dstPointer, srcPointer, sizeof(T) * chan);
        srcPointer += 4;
        dstPointer += chan;
    }
}

static constexpr char aboutData[] = {
    R"(<html><head/><body>
<p>
<b>JXL Frame Stitching</b>
<br>Join multiple images together into a single, multilayered or animated JPEG XL image
</p>
<p>Kampidh 2024
<br>Project github page: <a href="https://github.com/kampidh/JXL-Frame-Stitcher">https://github.com/kampidh/JXL-Frame-Stitcher</a></p>
<p>3rd party libraries used:</p>
<ul>
<li>libjxl 0.10.3</li>
<li>littlecms 2.16</li>
</ul>
</body></html>
)"};

static constexpr char basicUsage[] = {
    R"(<html><head/><body>
<p>
<b>Basic usage</b>
</p>
<ul>
<li>Add image files to the list by drag-and-drop or "Add Files..." button</li>
<li>Added files will be sorted alphabetically, you can reoder the frames by drag and drop on the Frame list</li>
<li>Select the image to change the frame settings, or you can also change multiple frames at once by multiple select them, and click apply</li>
<li>You can save and load current workspace settings from the File menu</li>
</ul>
<p><b>Selected Frame</b></p>
<ul>
<li><b>Save as reference frame 1</b>: sets the currently selected frame as libjxl reference frame 1 for blending operation</li>
<li><b>Frame duration</b>: sets the frame duration in ticks, if the input image is animated (eg. GIF) then this will be the delay for the last subframe before displaying the next frame</li>
<li><b>Frame reference</b>: selects the reference frame for the blending to take place</li>
<li><b>Frame anchor</b>: sets the top left (origin) position of current frame in relative to the first frame, can be negative (out of canvas)</li>
<li><b>Blend mode</b>: selects libjxl blending mode</li>
<li><b>Frame name</b>: sets the current frame name, this can be decoded as a layer name (eg. in Krita) when saved as a non-animated, multilayered JXL</li>
</ul>
<p><b>Global Setting</b></p>
<ul>
<li><b>Animated</b>: if unchecked, encoding result will be multilayer JXL with first frame become the bottom layer and last frame become the topmost layer.</li>
<li><b>Numerator/Denominator</b>: frames/second(s), for example 12 FPS will be 12/1</li>
<li><b>Loops</b>: number of loops, if set to 0 the result animated JXL will loop indefinitely</li>
<li><b>Distance</b>: sets the output image quality, 0 = lossless, range 0-25</li>
<li><b>Effort</b>: sets the libjxl encoding effort, range 1-10</li>
<li><b>Color space</b>: sets the output color space, "Inherit first image" will retain ICC profile of the first frame (if any) and convert subsequent frames to match the first,
"RAW" will not convert any frames, but still tag them as sRGB for displaying (useful if frames have different profiles and will be reassigned at postprocessing)</li>
<li><b>Bit depth</b>: sets the output bit depth per channel</li>
<li><b>Alpha channel</b>: if checked, output JXL will also save alpha channel</li>
<li><b>Alpha lossless</b>: if checked, alpha channel will set as lossless regardless of distance setting</li>
<li><b>Alpha premultiply</b>: sets the alpha premultiply flag on libjxl</li>
</ul>
<p>
<b>Limitations</b>
</p>
<ul>
<li>JXL input is coalesced</li>
</ul>
</body></html>
)"};
} // namespace jxfrstch

#endif // JXLUTILS_H
