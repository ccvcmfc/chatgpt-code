#ifndef NPZ_WRITER_QT_H
#define NPZ_WRITER_QT_H
#include <QImage>
#include <QString>
#include <memory>

// Qt 5.9 / C++11 / zlib. One ZIP32 data.npy entry, uint8 CHW.
// RGB32 input: black -> 0, any nonblack RGB -> 255.
// All methods must be called from the same thread.
class NpzWriter {
public:
    NpzWriter();
    ~NpzWriter();
    int begin(const QString& filePath, int channels, int height, int width);
    int append(const QImage& image);
    int end();
    void cancel();
private:
    struct State;
    std::unique_ptr<State> s_;
    NpzWriter(const NpzWriter&) = delete;
    NpzWriter& operator=(const NpzWriter&) = delete;
};
#endif
