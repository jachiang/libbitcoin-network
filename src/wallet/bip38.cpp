/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/bitcoin/wallet/bip38.hpp>

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <stdexcept>
#include <boost/locale.hpp>
#include <bitcoin/bitcoin/define.hpp>
#include <bitcoin/bitcoin/formats/base16.hpp>
#include <bitcoin/bitcoin/formats/base58.hpp>
#include <bitcoin/bitcoin/math/checksum.hpp>
#include <bitcoin/bitcoin/math/crypto.hpp>
#include <bitcoin/bitcoin/math/hash.hpp>
#include <bitcoin/bitcoin/math/ec_keys.hpp>
#include <bitcoin/bitcoin/unicode/unicode.hpp>
#include <bitcoin/bitcoin/utility/assert.hpp>
#include <bitcoin/bitcoin/utility/data.hpp>
#include <bitcoin/bitcoin/utility/endian.hpp>
#include <bitcoin/bitcoin/wallet/key_formats.hpp>
#include <bitcoin/bitcoin/wallet/payment_address.hpp>

namespace libbitcoin {
namespace bip38 {

using namespace bc::wallet;

static constexpr size_t two_block_size = bc::long_hash_size;
static constexpr size_t block_size = two_block_size / 2;
static constexpr size_t half = block_size / 2;
static constexpr size_t quarter = half / 2;

typedef byte_array<two_block_size> two_block;
typedef byte_array<block_size> full_block;
typedef byte_array<half> half_block;

// The above sizes are all tied to aes256.
static_assert(2 * quarter == bc::aes256_block_size, "oops!");

// Byte array semantic partitions.
namespace at
{
    // encrypted private key
    namespace private_key
    {
        static constexpr bounds prefix = { 0, 2 };        // 2
        static constexpr bounds flags = { 2, 3 };         // 1
        static constexpr bounds salt = { 3, 7 };          // 4
        static constexpr bounds entropy = { 7, 15 };      // 8
        static constexpr bounds part1 = { 15, 23 };       // 8
        static constexpr bounds part2 = { 23, 39 };       //16
        static constexpr bounds checksum = { 39, 43 };    // 4

        static constexpr bounds encrypted =               // 32
        { 
            entropy.start,
            part2.end 
        };

        static constexpr bounds version =                 // 1
        {
            prefix.start,
            prefix.start + 1
        };
    }

    // encrypted public key (AKA confirmation code)
    namespace public_key
    {
        static constexpr bounds prefix = { 0, 5 };        // 4
        static constexpr bounds flags = { 5, 6 };         // 1
        static constexpr bounds salt = { 6, 10 };         // 4
        static constexpr bounds entropy = { 10, 18 };     // 8
        static constexpr bounds sign = { 18, 19 };        // 1
        static constexpr bounds hash = { 19, 51 };        //32
        static constexpr bounds checksum = { 51, 55 };    // 4
    }

    // intermediate passphrase (with lot/sequence)
    namespace lot_token
    {
        static constexpr bounds prefix = { 0, 8 };        // 8
        static constexpr bounds salt = { 8, 12 };         // 4
        static constexpr bounds lot = { 12, 16 };         // 4
        static constexpr bounds sign = { 16, 17 };        // 1
        static constexpr bounds hash = { 17, 49 };        //32
        static constexpr bounds checksum = { 49, 53 };    // 4

        static constexpr bounds entropy =                 // 8
        {
            salt.start,
            lot.end
        };
    }

    // intermediate passphrase (without lot/sequence)
    namespace token
    {
        static constexpr bounds prefix = { 0, 8 };        // 8
        static constexpr bounds entropy = { 8, 16 };      // 8
        static constexpr bounds sign = { 16, 17 };        // 1
        static constexpr bounds hash = { 17, 49 };        //32
        static constexpr bounds checksum = { 49, 53 };    // 4

        // w/out lot salt is an alias for entropy and is 8 vs. usual 4 bytes
        static const bounds salt = entropy;               // 8
    }
}

// BIP38
// Alt-chain implementers should exploit the address hash for [identification].
// Since each operation in this proposal involves hashing a text representation
// of a coin address which (for Bitcoin) includes the leading '1', an alt-chain
// can easily be denoted simply by using the alt-chain's preferred format for
// representing an address.
//
// BIP38
// Alt-chain implementers may also change the prefix such that encrypted
// addresses do not start with "6P". [We do not currently support varying it.]
//
// The first byte in each prefix is also the base58check version byte.
namespace prefix
{
    // This prefix results in the prefix "6P" in the base58 encoding.
    static const data_chunk private_key
    {
        0x01, 0x42
    };

    // This prefix results in the prefix "6P" in the base58 encoding.
    static const data_chunk private_key_multiplied
    {
        0x01, 0x43
    };

    // This prefix results in the prefix "cfrm" in the base58 encoding.
    static const data_chunk public_key
    {
        0x64, 0x3b, 0xf6, 0xa8, 0x9a
    };

    // This prefix results in the prefix "passphrase" in the base58 encoding.
    static const data_chunk lot_token
    {
        0x2c, 0xe9, 0xb3, 0xe1, 0xff, 0x39, 0xe2, 0x51
    };

    // This prefix results in the prefix "passphrase" in the base58 encoding.
    static const data_chunk token
    {
        0x2c, 0xe9, 0xb3, 0xe1, 0xff, 0x39, 0xe2, 0x53
    };
}

// BIP38
// It is requested that the unused flag bytes NOT be used for denoting that
// the key belongs to an alt-chain [This shoud read "flag bits"?].
enum flag_byte : uint8_t
{
    none = 0,
    lot_sequence = 1 << 2,
    ec_compressed = 1 << 5,
    ec_non_multiplied_low = 1 << 6,
    ec_non_multiplied_high = 1 << 7,

    // Two bits are used to represent "not multiplied".
    ec_non_multiplied = (ec_non_multiplied_low | ec_non_multiplied_high)
};

static inline bool check_flag(data_slice key, const bounds& flags_position,
    flag_byte flag)
{
    BITCOIN_ASSERT(flags_position.start < key.size());
    return (key.data()[flags_position.start] & flag) != 0;
}

static inline uint8_t generate_flag_byte(bool multiplied, bool compressed,
    bool lot_sequence)
{
    uint8_t byte = flag_byte::none;
    if (lot_sequence)
        byte |= flag_byte::lot_sequence;

    if (compressed)
        byte |= flag_byte::ec_compressed;

    if (!multiplied)
        byte |= flag_byte::ec_non_multiplied;

    return byte;
}

static inline data_chunk new_flags(bool compressed)
{
    return
    {
        generate_flag_byte(false, compressed, false)
    };
}

static inline data_chunk new_flags(const token& token, bool compressed)
{
    const auto lot = prefix::lot_token;
    const auto actual = slice(token, at::token::prefix);
    const auto is_lot = std::equal(lot.begin(), lot.end(), actual.begin());
    return
    {
        generate_flag_byte(true, compressed, is_lot)
    };
}

static inline data_chunk point_sign(uint8_t byte, data_slice buffer)
{
    BITCOIN_ASSERT(!buffer.empty());
    static constexpr uint8_t low_bit_mask = 0x01;
    const uint8_t last_byte = buffer.data()[buffer.size() - 1];
    const uint8_t last_byte_odd_flag = last_byte & low_bit_mask;
    const uint8_t sign_byte = byte ^ last_byte_odd_flag;
    return
    {
        sign_byte
    };
}

// This provides a bi-directional mapping.
static inline uint8_t convert_version(const uint8_t version)
{
    switch (version)
    {
        case 0:
            return 1;
        case 1:
            return 0;
        default:
            return version;
    }
}

static inline uint8_t read_version(const private_key& key)
{
    // Infer the decrypt version from the private key prefix bytes.
    // This will operate just like compression inference. As such it will
    // require a mapping from 0x01 (private key) => 0x00 (address), becuase
    // unfortunately the authors don't appear to have considered that 
    // otherwise the decryption of private keys requires the key, passphrase
    // *and the version byte*. Also they used (01) for bitcoin addresess (00).
    // So in order to not waste a bit we special case 00|01 <-> 01|00.
    // All others map directly between address and bip38 private key.
    // We don't modify any other bip38 prefixes for altcoins and instead
    // rely on the address hash differentiation So "6P" can be replaced
    // deterministically and "cfrm" and "passphrase" are not impacted.
    const auto prefix_version = slice(key, at::private_key::version)[0];
    return convert_version(prefix_version);
}

static inline data_chunk versioned_prefix(const uint8_t address_version,
    const data_chunk& prefix)
{
    auto prefix_version = prefix;
    prefix_version[0] = convert_version(address_version);
    return prefix_version;
}

static data_chunk address_salt(uint8_t version, const ec_point& point)
{
    payment_address address(version, point);
    const auto hash = bitcoin_hash(to_data_chunk(address.to_string()));

    // data_chunk explicit here to avoid MSVC CTP compiler bug.
    return data_chunk
    {
        hash.begin(),
        hash.begin() + salt_size
    };
}

static void create_private_key(private_key& out_private, data_slice flags,
    data_slice salt, data_slice entropy, data_slice derived1,
    data_slice derived2, const seed& seed, uint8_t address_version)
{
    auto half1 = xor_data(seed, derived1, 0, half);
    aes256_encrypt(derived2, half1);

    auto combined = slice(half1, quarter, half);
    const auto seed_data = slice(seed, half, half + quarter);
    extend_data(combined, seed_data);

    auto half2 = xor_data(combined, derived1, 0, half, half);
    aes256_encrypt(derived2, half2);
    const auto quart1 = slice(half1, 0, quarter);

    const auto prefix = versioned_prefix(address_version,
        prefix::private_key_multiplied);

    build_checked_array(out_private,
    {
        prefix,
        flags,
        salt,
        entropy,
        quart1,
        half2
    });
}

static void create_public_key(public_key& out_public, data_slice flags,
    data_slice salt, data_slice entropy, data_slice derived1,
    data_slice derived2, const ec_secret& secret, bool compressed)
{
    const auto point = secret_to_public_key(secret, compressed);
    const data_chunk unsigned_point(point.begin() + 1, point.end());

    auto half1 = xor_data(unsigned_point, derived1, 0, half);
    aes256_encrypt(derived2, half1);

    auto half2 = xor_data(unsigned_point, derived1, half, half);
    aes256_encrypt(derived2, half2);

    const auto sign = point_sign(point.front(), derived2);

    build_checked_array(out_public,
    {
        prefix::public_key,
        flags,
        salt,
        entropy,
        sign,
        half1,
        half2
    });
}

bool create_key_pair(private_key& out_private, public_key& out_public,
    const token& token, const seed& seed, uint8_t address_version,
    bool compressed)
{
    if (!verify_checksum(token))
        return false;

    // This is the only place where we read a prefix for context.
    const auto flags = new_flags(token, compressed);
    auto pass_point = slice(token, at::token::hash);
    auto entropy = slice(token, at::token::entropy);

    auto point = pass_point;
    const ec_secret factor(bitcoin_hash(seed));
    ec_multiply(point, factor);
    if (!compressed)
        point = decompress_public_key(point);

    const auto salt = address_salt(address_version, point);
    auto salt_entropy = salt;
    extend_data(salt_entropy, entropy);

    two_block derived;
    if (!scrypt(pass_point, salt_entropy, derived))
        return false;

    data_chunk derived1;
    data_chunk derived2;
    split(derived, derived1, derived2, two_block_size);

    create_private_key(out_private, flags, salt, entropy, derived1, derived2,
        seed, address_version);

    create_public_key(out_public, flags, salt, entropy, derived1, derived2,
        factor, compressed);

    return true;
}

#ifdef WITH_ICU

// This call requires an ICU build, the other excluded calls are dependencies.
static inline data_chunk normal(const std::string& passphrase)
{
    return to_data_chunk(to_normal_nfc_form(passphrase));
}

bool create_token(token& out_token, const std::string& passphrase,
    const salt& salt, uint32_t lot, uint32_t sequence)
{
    if (lot > max_token_lot || sequence > max_token_sequence)
        return false;

    // Combine lot and sequence into 32 bits.
    static constexpr size_t max_sequence_bits = 12;
    const uint32_t lot_sequence = (lot << max_sequence_bits) || sequence;

    // Add big-endian lost/sequence to salt to create entropy.
    auto entropy = to_data_chunk(salt);
    extend_data(entropy, to_big_endian(lot_sequence));

    // Derive a key from the passphrase using scrypt.
    full_block pre_factor;
    if (!scrypt(normal(passphrase), salt, pre_factor))
        return false;

    ec_secret secret;
    auto pass_factor = to_data_chunk(pre_factor);
    extend_data(pass_factor, entropy);
    pass_factor = to_data_chunk(bitcoin_hash(pass_factor));
    std::copy(pass_factor.begin(), pass_factor.end(), secret.begin());

    ec_point pass_point;
    ///////////////////////////////////////////////////////////////////////////
    //// TODO: pass_point = compressed(G * passfactor) -> 33 bytes
    ///////////////////////////////////////////////////////////////////////////
    ec_multiply(pass_point, secret);

    build_array(out_token,
    {
        prefix::lot_token,
        entropy,
        pass_point
    });

    return true;
}

bool encrypt(private_key& out_private, const ec_secret& secret,
    const std::string& passphrase, uint8_t address_version, bool compressed)
{
    const auto point = secret_to_public_key(secret, compressed);
    const auto salt = address_salt(address_version, point);

    two_block derived;
    if (!scrypt(normal(passphrase), salt, derived))
        return false;

    data_chunk derived1;
    data_chunk derived2;
    split(derived, derived1, derived2, two_block_size);

    auto half1 = xor_data(secret, derived1, 0, half);
    aes256_encrypt(derived2, half1);

    auto half2 = xor_data(secret, derived1, half, half);
    aes256_encrypt(derived2, half2);

    const auto prefix = versioned_prefix(address_version, prefix::private_key);

    build_array(out_private,
    {
        prefix,
        new_flags(compressed),
        salt,
        half1,
        half2
    });

    return true;
}

static bool validate(const ec_secret& secret, data_slice salt,
    uint8_t address_version, bool compressed)
{
    // salt can be 4 or 8 bytes.
    const auto point = secret_to_public_key(secret, compressed);
    payment_address address(address_version, point);
    const auto hash = bitcoin_hash(to_data_chunk(address.to_string()));
    return std::equal(hash.begin(), hash.begin() + salt.size(), salt.begin());
}

static bool multiplied_secret(ec_secret& out_secret, const private_key& key,
    const std::string& passphrase,  uint8_t address_version)
{
    const bool lot = check_flag(key, at::private_key::flags,
        flag_byte::lot_sequence);
    const auto owner_salt_bound = lot ? at::lot_token::salt : 
        at::token::salt;

    const auto salt = slice(key, at::token::salt);
    const auto entropy = slice(key, at::token::entropy);
    const auto owner_salt = slice(key, owner_salt_bound);

    ec_secret secret;
    if (!scrypt(normal(passphrase), owner_salt, secret))
        return false;

    if (lot)
    {
        auto extended_secret = to_data_chunk(secret);
        extend_data(extended_secret, entropy);
        secret = bitcoin_hash(extended_secret);
    }

    const bool compressed = check_flag(key, at::private_key::flags,
        flag_byte::ec_compressed);
    const auto point = secret_to_public_key(secret, compressed);

    two_block seed_pass;
    if (!scrypt(point, entropy, seed_pass))
        return false;

    data_chunk derived1;
    data_chunk derived2;
    split(seed_pass, derived1, derived2, two_block_size);

    auto part1 = slice(key, at::private_key::part1);
    auto part2 = slice(key, at::private_key::part2);

    aes256_decrypt(derived2, part2);
    auto xor_seed = xor_data(part2, derived2, 0, half, half);

    data_chunk seed_part;
    data_chunk remainder_part;
    split(xor_seed, remainder_part, seed_part, half);

    extend_data(part1, remainder_part);
    aes256_decrypt(derived2, part1);

    auto seed = xor_data(part1, derived1, 0, half);
    extend_data(seed, seed_part);
    const auto factor = ec_secret(bitcoin_hash(seed));

    ec_multiply(secret, factor);

    if (!validate(secret, salt, address_version, compressed))
        return false;

    out_secret = secret;
    return true;
}

static bool secret(ec_secret& out_secret, const private_key& key,
    const std::string& passphrase, uint8_t address_version)
{
    const auto salt = slice(key, at::private_key::salt);
    const auto encrypted = slice(key, at::private_key::encrypted);
    const bool compressed = check_flag(key, at::private_key::flags,
        flag_byte::ec_compressed);

    two_block derived_data;
    if (!scrypt(normal(passphrase), salt, derived_data))
        return false;

    data_chunk data1;
    data_chunk data2;
    split(encrypted, data1, data2, block_size);

    data_chunk derived1;
    data_chunk derived2;
    split(derived_data, derived1, derived2, two_block_size);

    aes256_decrypt(derived2, data1);
    aes256_decrypt(derived2, data2);

    const auto combined = build_data({ data1, data2 });
    const auto decrypted = xor_data(combined, derived1, 0, block_size);

    ec_secret secret;
    std::copy(decrypted.begin(), decrypted.end(), secret.begin());

    if (!validate(secret, salt, address_version, compressed))
        return false;

    out_secret = secret;
    return true;
}

bool decrypt(ec_secret& out_secret, const private_key& key,
    const std::string& passphrase)
{
    if (!verify_checksum(key))
        return false;

    const auto address_version = read_version(key);
    const bool multiplied = !check_flag(key, at::private_key::flags,
        flag_byte::ec_non_multiplied);

    if (multiplied)
        return multiplied_secret(out_secret, key, passphrase, address_version);
    else
        return secret(out_secret, key, passphrase, address_version);
}

bool decrypt(ec_point& out_point, const public_key& key,
    const std::string& passphrase)
{
    if (!verify_checksum(key))
        return false;

    const bool lot = check_flag(key, at::public_key::flags,
        flag_byte::lot_sequence);
    const bool compressed = check_flag(key, at::public_key::flags,
        flag_byte::ec_compressed);

    const auto owner_salt_bound = lot ? at::lot_token::salt : 
        at::token::salt;

    const auto key_sign = slice(key, at::public_key::sign);
    const auto hash = slice(key, at::public_key::hash);
    const auto owner_salt = slice(key, owner_salt_bound);

    auto salt_entropy = slice(key, at::public_key::salt);
    const auto entropy = slice(key, at::public_key::entropy);
    extend_data(salt_entropy, entropy);

    hash_digest pre_factor;
    if (!scrypt(normal(passphrase), owner_salt, pre_factor))
        return false;

    ec_secret pass_factor;
    if (lot)
    {
        auto extended_pre_factor = to_data_chunk(pre_factor);
        extend_data(extended_pre_factor, entropy);
        pre_factor = bitcoin_hash(extended_pre_factor);
    }
    
    pass_factor = ec_secret(pre_factor);
    const auto pass_point = secret_to_public_key(pass_factor, true);

    two_block derived_data;
    if (!scrypt(pass_point, salt_entropy, derived_data))
        return false;

    data_chunk derived1;
    data_chunk derived2;
    split(derived_data, derived1, derived2, two_block_size);

    data_chunk encrypted1;
    data_chunk encrypted2;
    split(hash, encrypted1, encrypted2, block_size);

    aes256_decrypt(derived2, encrypted1);
    auto decrypted1 = xor_data(encrypted1, derived1, 0, half);

    aes256_decrypt(derived2, encrypted2);
    auto decrypted2 = xor_data(encrypted2, derived1, 0, half, half);

    const auto sign = point_sign(key_sign.front(), derived2);

    out_point.clear();
    extend_data(out_point, sign);
    extend_data(out_point, decrypted1);
    extend_data(out_point, decrypted2);
    ec_multiply(out_point, pass_factor);
    if (!compressed)
        decompress_public_key(out_point);

    return true;
}

#endif // WITH_ICU

} // namespace bip38
} // namespace libbitcoin
