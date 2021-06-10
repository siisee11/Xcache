#ifndef CountingBloomFilter_H
#define CountingBloomFilter_H

#include <vector>
#include <cstdbool>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <bitset>
#include <string.h>
#include <mutex>
#include <openssl/sha.h>
#include "util/hash.h"

#define BITS_PER_BYTE           8
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#define BITS_TO_LONGS(nr)       DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

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
			CountingBloomFilter(uint8_t numHashes, uint64_t numBits)
			: m_numHashes(numHashes), m_numBits(numBits), m_numLongs(BITS_TO_LONGS(numBits))
			{
				m_bitarray = (uint8_t *)calloc(numBits , sizeof(uint8_t));
				m_boolbitarray = (uint64_t *)calloc(BITS_TO_LONGS(numBits), sizeof(uint64_t));
			}

		/** Returns the number of hashes used by this Bloom filter
		*/
		uint8_t GetNumHashes() const {
			return m_numHashes;
		}

		uint64_t GetBaseAddr() const {
			return (uint64_t)m_bitarray;
		}

		/** Returns the number of bits in the bit array used by this Bloom filter.
		 *  The value does not reflect the actual storage size of the BF, but rather
		 *  is related to the false-positive rate of the BF.
		 *  
		 * @see     AbstractBloomFilter::AbstractBloomFilter
		 * @return  Number of bits, in terms of an ordinary Bloom filter
		 */
		uint64_t GetNumBits() const {
			return m_numBits;
		}

		uint64_t GetNumLongs() const {
			return m_numLongs;
		}

		uint64_t GetBoolBitArray() const {
			return (uint64_t)m_boolbitarray;
		}

		uint64_t GetLong(int idx) const {
			return m_boolbitarray[idx];
		}

		void Insert(T const& o) {
			for(uint8_t i = 0; i < GetNumHashes(); i++){
				auto idx = ComputeHash(o, i);
//				printf("idx: %d\n", idx);
				if (m_bitarray[idx] < 255)
					m_bitarray[idx] += 1;
				else
					printf("idx: %lu exceed 256\n", idx);
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
//			printf("Query : ");
			for(uint8_t i = 0; i < GetNumHashes(); i++){
				auto idx = ComputeHash(o, i);
//				printf("%lu, ", idx);
				if(m_bitarray[idx] == 0){
					return false;
				}
			}
			return true;
		}

		bool QueryBitBloom(T const& o) const {
//			printf("Query BB : ");
			for(uint8_t i = 0; i < GetNumHashes(); i++){
				auto idx = ComputeHash(o, i);
//				printf("%lu, ", idx);
				auto j = idx / 64; 
				uint8_t bitShift = 64 - 1 - (idx % 64);   // i == 65 -> 62 
				uint64_t checkBit = (uint64_t) 1 << bitShift;
				if ((m_boolbitarray[j] & checkBit) == 0) {
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

		/** Returns an ordinary BF with the same set represented by this counting
		 *  BF.
		 *
		 *  @return The new OrdinaryBloomFilter
		 */
		void ToOrdinaryBloomFilter() const {
			for(uint64_t i = 0; i < m_numLongs; i++){
				m_boolbitarray[i] = 0;
			}
			for(uint64_t i = 0; i < GetNumBits(); i++){
				auto j = i / 64; 
				if (m_bitarray[i] > 0) {
					uint8_t bitShift = 64 - 1 - (i % 64);   // i == 65 -> 62 
					uint64_t checkBit = (uint64_t) 1 << bitShift;
					m_boolbitarray[j] |= checkBit;
				}
			}
			return ;
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

		uint64_t ComputeHash(T const& key, uint8_t salt) const {
//			auto f_hash = hash_funcs[salt](&key, sizeof(key), 0xc70f6907UL);
			auto f_hash = hash_funcs[1](&key, sizeof(key), salt);
			int idx = f_hash % GetNumBits();
			return idx;
		}

	private:

		/** Number of hashes
		*/
		uint8_t m_numHashes;

		/** Number of bits for bit array
		*/
		uint64_t m_numBits;
		uint64_t m_numLongs;

//		std::vector<uint8_t> m_bitarray;
		uint8_t *m_bitarray;

		uint64_t *m_boolbitarray;


}; // class CountingBloomFilter

#endif
