/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#pragma once
#include <array>
#include <stdint.h>

namespace catapult { namespace utils {

#pragma pack(push, 1)

	/// Structure composed of a single byte.
	struct UnresolvedAddressByte {
	public:
		/// Memory backed byte.
		uint8_t Byte;

	public:
		/// Returns \c true if this byte is equal to \a rhs.
		constexpr bool operator==(const UnresolvedAddressByte& rhs) const {
			return Byte == rhs.Byte;
		}

		/// Returns \c true if this byte is not equal to \a rhs.
		constexpr bool operator!=(const UnresolvedAddressByte& rhs) const {
			return !(*this == rhs);
		}
	};

#pragma pack(pop)

	constexpr size_t Address_Decoded_Size = 25;
	constexpr size_t Address_Encoded_Size = 40;

	/// Address.
	using Address = std::array<uint8_t, Address_Decoded_Size>;

	/// Unresolved address.
	using UnresolvedAddress = std::array<UnresolvedAddressByte, Address_Decoded_Size>;
}}
