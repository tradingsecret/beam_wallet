// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "common.h"

namespace Eth
{

	void MemCopy(void* dest, const void* src, uint32_t n)
	{
#ifdef HOST_BUILD
		memcpy(dest, src, n);
#else // HOST_BUILD
		Env::Memcpy(dest, src, n);
#endif // HOST_BUILD
	}


	template <uint32_t N>
	constexpr uint32_t strlen(char const (&s)[N])
	{
		return N - 1;
	}

	Opaque<1> to_opaque(char s)
	{
		Opaque<1> r;
		MemCopy(&r, &s, 1);
		return r;
	}

	template<uint32_t N>
	auto to_opaque(char const (&s)[N])
	{
		constexpr auto size = sizeof(char) * (N - 1);
		Opaque<size> r;
		MemCopy(&r, &s, size);
		return r;
	}

	struct Rlp
	{
		struct Node
		{
			enum struct Type {
				List,
				String,
				Integer,
			};

			Type m_Type;
			mutable uint64_t m_SizeBrutto = 0; // cached
			uint32_t m_nLen; // for strings and lists

			union {
				const Node* m_pC;
				const uint8_t* m_pBuf;
				uint64_t m_Integer;
			} ;

			template <uint32_t nBytes>
			explicit Node(const Opaque<nBytes>& hv)
				: m_Type(Type::String)
				, m_nLen(nBytes)
				, m_pBuf(reinterpret_cast<const uint8_t*>(&hv))
			{
			}

			Node() = default;

			explicit Node(uint64_t n)
				: m_Type(Type::Integer)
				, m_nLen(0)
				, m_Integer(n)
			{
			}

			template <uint32_t N>
			Node(const Node(&nodes)[N])
				: m_Type(Type::List)
				, m_nLen(N)
				, m_pC(nodes)
			{
			}

			template <uint32_t nBytes>
			void Set(const Opaque<nBytes>& hv)
			{
				m_Type = Type::String;
				m_nLen = nBytes;
				m_pBuf = reinterpret_cast<const uint8_t*>(&hv);
			}

			void Set(uint64_t n)
			{
				m_Type = Type::Integer;
				m_Integer = n;
			}

			static constexpr uint8_t get_BytesFor(uint64_t n)
			{
				uint8_t nLen = 0;
				while (n)
				{
					n >>= 8;
					nLen++;

				}
				return nLen;
			}

			void EnsureSizeBrutto() const
			{
				if (!m_SizeBrutto)
				{
					struct SizeCounter {
						uint64_t m_Val = 0;
						void Write(uint8_t) { m_Val++; }
						void Write(const void*, uint32_t nLen) { m_Val += nLen; }
					} sc;

					Write(sc);
					m_SizeBrutto = sc.m_Val;
				}
			}

			template <typename TStream>
			static void WriteVarLen(TStream& s, uint64_t n, uint8_t nLen)
			{
				for (nLen <<= 3; nLen; )
				{
					nLen -= 8;
					s.Write(static_cast<uint8_t>(n >> nLen));
				}
			}

			template <typename TStream>
			void WriteSize(TStream& s, uint8_t nBase, uint64_t n) const
			{
				if (n < 56)
					s.Write(nBase + static_cast<uint8_t>(n));
				else
				{
					uint8_t nLen = get_BytesFor(n);
					s.Write(nBase + 55 + nLen);
					WriteVarLen(s, n, nLen);
				}
			}

			template <typename TStream>
			void Write(TStream& s) const
			{
				switch (m_Type)
				{
				case Type::List:
					{
						uint64_t nChildren = 0;

						for (uint32_t i = 0; i < m_nLen; i++)
						{
							m_pC[i].EnsureSizeBrutto();
							nChildren += m_pC[i].m_SizeBrutto;
						}

						WriteSize(s, 0xc0, nChildren);

						for (uint32_t i = 0; i < m_nLen; i++)
							m_pC[i].Write(s);

					}
					break;

				case Type::String:
					{
						if (m_nLen != 1 || m_pBuf[0] >= 0x80)
						{
							WriteSize(s, 0x80, m_nLen);
						}
						s.Write(m_pBuf, m_nLen);
					}
					break;

				default:
					assert(false);
					// no break;

				case Type::Integer:
					{
						uint8_t nLen = get_BytesFor(m_Integer);
						WriteSize(s, 0x80, nLen);
						WriteVarLen(s, m_Integer, nLen);
					}
				}
			}
		};

		template<typename Visitor>
		static bool Decode(const uint8_t* input, uint32_t size, Visitor& visitor)
		{
			uint32_t position = 0;
			auto decodeInteger = [&](uint8_t nBytes, uint32_t& length)
			{
				if (nBytes > size - position)
					return false; 
				length = 0;
				while (nBytes--)
				{
					length = input[position++] + length * 256;
				}
				return true;
			};

			while (position < size)
			{
				auto b = input[position++];
				if (b <= 0x7f)  // single byte
				{
					visitor.OnNode(Rlp::Node(to_opaque(b)));
				}
				else
				{
					uint32_t length = 0;
					if (b <= 0xb7) // short string
					{
						length = b - 0x80;
						if (length > size - position)
							return false;
						DecodeString(input + position, length, visitor);
					}
					else if (b <= 0xbf) // long string
					{
						if (!decodeInteger(b - 0xb7, length) || length > size - position)
							return false;
						DecodeString(input + position, length, visitor);
					}
					else if (b <= 0xf7) // short list
					{
						length = b - 0xc0;
						if (length > size - position)
							return false;
						if (!DecodeList(input + position, length, visitor))
							return false;
					}
					else if (b <= 0xff) // long list
					{
						if (!decodeInteger(b - 0xf7, length) || length > size - position)
							return false;
						if (!DecodeList(input + position, length, visitor))
							return false;
					}
					else
					{
						return false;
					}
					position += length;
				}
			}
			return true;
		}

		template <typename Visitor>
		static void DecodeString(const uint8_t* input, uint32_t size, Visitor& visitor)
		{
			Rlp::Node n;
			n.m_Type = Rlp::Node::Type::String;
			n.m_nLen = size;
			n.m_pBuf = input;
			visitor.OnNode(n);
		}

		template <typename Visitor>
		static bool DecodeList(const uint8_t* input, uint32_t size, Visitor& visitor)
		{
			Rlp::Node n;
			n.m_Type = Rlp::Node::Type::List;
			n.m_nLen = size;
			n.m_pBuf = input;
			if (visitor.OnNode(n))
			{
				return Decode(input, size, visitor);
			}
			return true;
		}

		struct HashStream
		{

#ifdef HOST_BUILD
			beam::KeccakProcessor<256> m_hp;
#else // HOST_BUILD
			HashProcessor::Base m_hp;

			HashStream() {
				m_hp.m_p = Env::HashCreateKeccak(256);
			}
#endif // HOST_BUILD
		
			void Write(uint8_t x)
			{
				if (m_nBuf == _countof(m_pBuf))
					FlushStrict();

				m_pBuf[m_nBuf++] = x;

			}

			void Write(const uint8_t* p, uint32_t n)
			{
				if (!Append(p, n))
				{
					Flush();
					if (!Append(p, n))
						m_hp.Write(p, n);
				}
			}

			template <typename T>
			void operator >> (T& res)
			{
				if (m_nBuf)
					FlushStrict();
				m_hp >> res;
			}

		private:

			uint8_t m_pBuf[128];
			uint32_t m_nBuf = 0;

			void Flush()
			{
				if (m_nBuf)
					FlushStrict();
			}

			void FlushStrict()
			{
				m_hp.Write(m_pBuf, m_nBuf);
				m_nBuf = 0;
			}

			bool Append(const uint8_t* p, uint32_t n)
			{
				if (m_nBuf + n > _countof(m_pBuf))
					return false;

#ifdef HOST_BUILD
				memcpy(m_pBuf + m_nBuf, p, n);
#else // HOST_BUILD
				Env::Memcpy(m_pBuf + m_nBuf, p, n);
#endif // HOST_BUILD

				m_nBuf += n;
				return true;
			}
		};
	};

	struct Header
	{
		Opaque<32> m_ParentHash;
		Opaque<32> m_UncleHash;
		Opaque<20> m_Coinbase;
		Opaque<32> m_Root;
		Opaque<32> m_TxHash;
		Opaque<32> m_ReceiptHash;
		Opaque<256> m_Bloom;
		Opaque<32> m_Extra;
		uint32_t m_nExtra; // can be less than maximum size

		uint64_t m_Difficulty;
		uint64_t m_Number; // height
		uint64_t m_GasLimit;
		uint64_t m_GasUsed;
		uint64_t m_Time;
		uint64_t m_Nonce;

		void get_HashForPow(Opaque<32>& hv) const
		{
			get_HashInternal(hv, nullptr);
		}

		void get_HashFinal(Opaque<32>& hv, const Opaque<32>& hvMixHash) const
		{
			get_HashInternal(hv, &hvMixHash);
		}

		void get_SeedForPoW(Opaque<64>& hv) const
		{
			Opaque<32> x;
			get_HashForPow(x);

#ifdef HOST_BUILD
			beam::KeccakProcessor<512> hp;
#else // HOST_BUILD
			HashProcessor::Base hp;
			hp.m_p = Env::HashCreateKeccak(512);
#endif // HOST_BUILD

			hp << x;

			auto nonce = Utils::FromLE(m_Nonce);
			hp.Write(&nonce, sizeof(nonce));

			hp >> hv;
		}

		uint32_t get_Epoch() const
		{
			return static_cast<uint32_t>(m_Number / 30000);
		}

	private:

		void get_HashInternal(Opaque<32>& hv, const Opaque<32>* phvMix) const
		{
			Rlp::Node pN[15];
			pN[0].Set(m_ParentHash);
			pN[1].Set(m_UncleHash);
			pN[2].Set(m_Coinbase);
			pN[3].Set(m_Root);
			pN[4].Set(m_TxHash);
			pN[5].Set(m_ReceiptHash);
			pN[6].Set(m_Bloom);
			pN[7].Set(m_Difficulty);
			pN[8].Set(m_Number);
			pN[9].Set(m_GasLimit);
			pN[10].Set(m_GasUsed);
			pN[11].Set(m_Time);
			pN[12].Set(m_Extra);
			pN[12].m_nLen = m_nExtra;

			Rlp::Node nRoot;
			nRoot.m_Type = Rlp::Node::Type::List;
			nRoot.m_pC = pN;
			if (phvMix)
			{
				pN[13].Set(*phvMix);
				pN[14].Set(m_Nonce);
				nRoot.m_nLen = 15;
			}
			else
				nRoot.m_nLen = 13;


			Rlp::HashStream hs;
			nRoot.Write(hs);
			hs >> hv;
		}
	};

}