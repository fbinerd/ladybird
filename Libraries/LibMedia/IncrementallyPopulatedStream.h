#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <Libraries/LibMedia/MediaStream.h>

namespace Media {

class IncrementallyPopulatedStreamCursor final : public MediaStreamCursor {
public:
    virtual ~IncrementallyPopulatedStreamCursor() override = default;
    virtual DecoderErrorOr<size_t> read_into(Bytes bytes) override { 
        return bytes.size(); 
    }
    virtual DecoderErrorOr<void> seek(i64, SeekMode) override { return {}; }
    virtual size_t position() const override { return 0; }
    virtual size_t size() const override { return 0; }
};

class IncrementallyPopulatedStream final : public MediaStream {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create_empty() {
        return adopt_ref(*new IncrementallyPopulatedStream());
    }
    
    virtual ~IncrementallyPopulatedStream() override = default;

    void add_chunk_at(unsigned long, ReadonlyBytes) { }
    size_t next_chunk_start() const { return 0; }
    void set_expected_size(unsigned long size) { m_expected_size = size; }
    size_t expected_size() const { return m_expected_size.value_or(0); }
    void close() { }

    template<typename T>
    void set_data_request_callback(T&& callback) { (void)callback; }

    virtual NonnullRefPtr<MediaStreamCursor> create_cursor() override {
        return adopt_ref(*new IncrementallyPopulatedStreamCursor());
    }

private:
    IncrementallyPopulatedStream() = default;
    Optional<size_t> m_expected_size;
};

}
