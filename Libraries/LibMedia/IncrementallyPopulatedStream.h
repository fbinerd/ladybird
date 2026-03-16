#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <Libraries/LibMedia/MediaStream.h>

namespace Media {

class __attribute__((visibility("default"))) IncrementallyPopulatedStream final : public MediaStream {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create_empty();

    void add_chunk_at(size_t offset, ReadonlyBytes);
    void close();
    void set_expected_size(size_t);
    
    // Removido o override para evitar erro de tipo (u64 vs size_t)
    size_t expected_size() const;
    size_t next_chunk_start() const;
    
    Optional<ByteString> url_hint() const { return m_url_hint; }
    void set_url_hint(ByteString url) { m_url_hint = move(url); }
    Optional<ByteString> mime_type_hint() const { return m_mime_type_hint; }
    void set_mime_type_hint(ByteString mime) { m_mime_type_hint = move(mime); }

    using DataRequestCallback = Function<void(size_t)>;
    void set_data_request_callback(DataRequestCallback);

    // Se o MediaStream não tiver um create_cursor estático, vamos definir um aqui
    virtual NonnullRefPtr<MediaStreamCursor> create_cursor() override;

    virtual ~IncrementallyPopulatedStream() override = default;

private:
    IncrementallyPopulatedStream() = default;
    
    Optional<size_t> m_expected_size;
    Optional<ByteString> m_url_hint;
    Optional<ByteString> m_mime_type_hint;
    DataRequestCallback m_data_request_callback;
};

}
