#include <AK/Debug.h>
#include <Libraries/LibMedia/IncrementallyPopulatedStream.h>

namespace Media {

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_empty()
{
    return adopt_ref(*new IncrementallyPopulatedStream());
}

void IncrementallyPopulatedStream::add_chunk_at(size_t offset, ReadonlyBytes bytes)
{
    (void)offset;
    (void)bytes;
}

size_t IncrementallyPopulatedStream::next_chunk_start() const { return 0; }

void IncrementallyPopulatedStream::set_expected_size(size_t size) { m_expected_size = size; }

size_t IncrementallyPopulatedStream::expected_size() const { return m_expected_size.value_or(0); }

void IncrementallyPopulatedStream::set_data_request_callback(DataRequestCallback callback) { m_data_request_callback = move(callback); }

// Usando TODO() para evitar o erro de instanciação de classe abstrata
NonnullRefPtr<MediaStreamCursor> IncrementallyPopulatedStream::create_cursor()
{
    TODO();
}

void IncrementallyPopulatedStream::close() {}

}
