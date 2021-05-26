#ifndef CountingBloomFilter_H
#define CountingBloomFilter_H

#include <vector>
#include <cstdbool>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <string.h>
#include <mutex>
#include <openssl/sha.h>
#include "util/hash.h"

/** Container type for a hashable object and a salt
 *  Do not use this struct directly, but rather its alias bloom::HashParams<T>.
 *
 *  @param T Contained type being hashed
 */
template <typename T>
struct HashParams_S {
	T a;        //!< Object to hash
	uint8_t b;  //!< 8-bit salt
};

/** Container type for a hashable object and a salt
 *
 *  @param T Contained type being hashed
 */
template <typename T>
using HashParams = struct HashParams_S;

/** A counting Bloom filter. Instead of an array of bits, maintains an array of
 *  bytes. Each byte is incremented for an added item, or decremented for a
 *  deleted item, thereby supporting a delete operation.
 *
 *  @param T Contained type being indexed
 */
template <typename T>
class CountingBloomFilter {

	public:

		/** Constructor: Creates a BloomFilter with the requested bit array size
		 *  and number of hashes.
		 *  
		 *  @param numHashes The number of hashes which will be computed for each
		 *                   added object.
		 *  @param numBits   The size of the bit array to allocate. If the
		 *                   BloomFilter is not an OrdinaryBloomFilter, the actual
		 *                   storage size will differ from this.
		 */
		explicit
			CountingBloomFilter(uint8_t numHashes, uint32_t numBits)
			: m_numHashes(numHashes), m_numBits(numBits)
			{
				m_bitarray.reserve(numBits);
				for(uint32_t i = 0; i < GetNumBits(); i++){
					m_bitarray[i] = 0;
				}
			}

		/** Returns the number of hashes used by this Bloom filter
		*/
		uint8_t GetNumHashes() const {
			return m_numHashes;
		}

		/** Returns the number of bits in the bit array used by this Bloom filter.
		 *  The value does not reflect the actual storage size of the BF, but rather
		 *  is related to the false-positive rate of the BF.
		 *  
		 * @see     AbstractBloomFilter::AbstractBloomFilter
		 * @return  Number of bits, in terms of an ordinary Bloom filter
		 */
		uint32_t GetNumBits() const {
			return m_numBits;
		}

		void Insert(T const& o) {

			for(uint8_t i = 0; i < GetNumHashes(); i++){
				int idx = ComputeHash(o, i);
				if (m_bitarray[idx] < 255)
					m_bitarray[idx] += 1;
				else
					printf("idx: %d exceed 256\n", idx);
			}
		}

		bool Delete(T const& o) {
			if(Query(o)){
				for(uint8_t i = 0; i < GetNumHashes(); i++){
					m_bitarray[ComputeHash(o, i)] -= 1;
				}
				return true;
			}
			return false;
		}

		bool Query(T const& o) const {
			for(uint8_t i = 0; i < GetNumHashes(); i++){
				int idx = ComputeHash(o, i);
				if(m_bitarray[idx] == 0){
					return false;
				}
			}
			return true;
		}

		virtual void Serialize(std::ostream &os) const {
			uint8_t numHashes = GetNumHashes();
			uint16_t numBits = GetNumBits();

			os.write((const char *) &numHashes, sizeof(uint8_t));
			os.write((const char *) &numBits, sizeof(uint16_t));

			for(uint16_t i = 0; i < numBits; i++){
				uint8_t byte = m_bitarray[i];
				os.write((const char *) &byte, sizeof(uint8_t));
			}
		}

		/** Create a CountingBloomFilter from the content of a binary input
		 * stream. No validation is performed.
		 *
		 * @param  is Input stream to read from
		 * @return Deserialized CountingBloomFilter
		 */
		static CountingBloomFilter<T> Deserialize(std::istream &is){
			uint8_t numHashes;
			uint16_t numBits;

			is.read((char *) &numHashes, sizeof(uint8_t));
			is.read((char *) &numBits, sizeof(uint16_t));

			CountingBloomFilter<T> r (numHashes, numBits);

			for(uint16_t i = 0; i < numBits; i++){
				uint8_t byte;
				is.read((char *) &byte, sizeof(uint8_t));
				r.m_bitarray[i] = byte;
			}

			return r;
		}

	protected:

		/** Returns the bit array index associated with the given (object, salt)
		 *  pair. Result is guaranteed to be between 0 and GetNumBits() - 1
		 *  (inclusive).
		 *
		 *  @param  o    Object to hash
		 *  @param  salt Salt to allow creating multiple hashes for an object
		 *  @return Index in bit array corresponding to the (object, salt) pair
		 */
		uint16_t ComputeSHA256(T const& o, uint8_t salt) const {
			unsigned char hash[SHA256_DIGEST_LENGTH];
			std::string str  = o + std::to_string(salt);
			const char * c = str.c_str();

			SHA256_CTX sha256;
			SHA256_Init(&sha256);
			SHA256_Update(&sha256, c, str.size());
			SHA256_Final(hash, &sha256);

			char outputBuffer[17];
			int i = 0;
			for (i = 0; i < SHA256_DIGEST_LENGTH/4; i++)
			{
				sprintf(outputBuffer + (i * 2),"%02x", hash[i]);
			}
			outputBuffer[16] = 0;
			int idx = atoi(outputBuffer) % GetNumBits();

			return idx;
		}

		uint16_t ComputeHash(T const& key, uint8_t salt) const {
			auto f_hash = hash_funcs[salt](&key, sizeof(key), 0xc70f6907UL);
			int idx = f_hash % GetNumBits();
			return idx;
		}

	private:

		/** Number of hashes
		*/
		uint8_t m_numHashes;

		/** Number of bits for bit array
		*/
		uint32_t m_numBits;

		std::vector<uint8_t> m_bitarray;

}; // class CountingBloomFilter

#endif
