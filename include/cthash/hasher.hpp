#ifndef CONSTEXPR_SHA2_HASHER_HPP
#define CONSTEXPR_SHA2_HASHER_HPP

#include "value.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <cassert>
#include <concepts>
#include <cstdint>

namespace cthash {

template <typename T> concept one_byte_char = (sizeof(T) == 1zu);

template <typename T> concept byte_like = (sizeof(T) == 1zu) && (std::same_as<T, char> || std::same_as<T, unsigned char> || std::same_as<T, char8_t> || std::same_as<T, std::byte> || std::same_as<T, uint8_t> || std::same_as<T, int8_t>);

template <typename T> concept string_literal = requires(const T & in) //
{
	[]<one_byte_char CharT, size_t N>(const CharT(&)[N]) {}(in);
};

template <typename T> concept convertible_to_byte_span = requires(T && obj) //
{
	{ std::span(obj) };
	requires byte_like<typename decltype(std::span(obj))::value_type>;
	requires !string_literal<T>;
};

template <typename It1, typename It2, typename It3> constexpr auto byte_copy(It1 first, It2 last, It3 destination) {
	return std::transform(first, last, destination, [](byte_like auto v) { return static_cast<std::byte>(v); });
}

template <std::unsigned_integral T> struct unwrap_bigendian_number {
	static constexpr size_t bytes = sizeof(T);
	static constexpr size_t bits = bytes * 8zu;

	std::span<std::byte, bytes> ref;

	constexpr void operator=(T value) noexcept {
		[&]<size_t... Idx>(std::index_sequence<Idx...>) {
			((ref[Idx] = static_cast<std::byte>(value >> ((bits - 8zu) - 8zu * Idx))), ...);
		}
		(std::make_index_sequence<bytes>());
	}
};

unwrap_bigendian_number(std::span<std::byte, 8>)->unwrap_bigendian_number<uint64_t>;
unwrap_bigendian_number(std::span<std::byte, 4>)->unwrap_bigendian_number<uint32_t>;

template <typename T> constexpr auto cast_from_bytes(std::span<const std::byte, sizeof(T)> in) noexcept {
	return [&]<size_t... Idx>(std::index_sequence<Idx...>) {
		return ((static_cast<T>(in[Idx]) << ((sizeof(T) - 1zu - Idx) * 8zu)) | ...);
	}
	(std::make_index_sequence<sizeof(T)>());
}

template <typename Config> struct internal_hasher {
	static constexpr auto config = Config{};
	static constexpr size_t block_size_bytes = config.block_bits / 8zu;

	// internal types
	using state_value_t = std::remove_cvref_t<decltype(Config::initial_values)>;
	using state_item_t = typename state_value_t::value_type;

	using block_value_t = std::array<std::byte, block_size_bytes>;
	using block_view_t = std::span<const std::byte, block_size_bytes>;

	using staging_item_t = typename decltype(config.constants)::value_type;
	static constexpr size_t staging_size = config.constants.size();
	using staging_value_t = std::array<staging_item_t, staging_size>;
	using staging_view_t = std::span<const staging_item_t, staging_size>;

	using digest_span_t = std::span<std::byte, config.digest_length>;
	using result_t = cthash::tagged_hash_value<Config>;
	using length_t = typename Config::length_type;

	// internal state
	state_value_t hash;
	length_t total_length;

	block_value_t block;
	unsigned block_used;

	// constructors
	constexpr internal_hasher() noexcept: hash{config.initial_values}, total_length{0zu}, block_used{0u} { }
	constexpr internal_hasher(const internal_hasher &) noexcept = default;
	constexpr internal_hasher(internal_hasher &&) noexcept = default;
	constexpr ~internal_hasher() noexcept = default;

	// take buffer and build staging
	static constexpr auto build_staging(block_view_t chunk) noexcept -> staging_value_t {
		[[clang::uninitialized]] staging_value_t w;

		constexpr auto first_part_size = block_size_bytes / sizeof(staging_item_t);

		// fill first part with chunk
		for (int i = 0; i != int(first_part_size); ++i) {
			w[i] = cast_from_bytes<staging_item_t>(chunk.subspan(i * sizeof(staging_item_t)).template first<sizeof(staging_item_t)>());
		}

		// fill the rest (generify)
		for (int i = int(first_part_size); i != int(staging_size); ++i) {
			const staging_item_t s0 = std::rotr(w[i - 15], config.staging_constants[0]) xor std::rotr(w[i - 15], config.staging_constants[1]) xor (w[i - 15] >> config.staging_constants[2]);
			const staging_item_t s1 = std::rotr(w[i - 2], config.staging_constants[3]) xor std::rotr(w[i - 2], config.staging_constants[4]) xor (w[i - 2] >> config.staging_constants[5]);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		return w;
	}

	static constexpr void rounds(staging_view_t w, state_value_t & state) noexcept {
		// create copy of internal state
		auto wvar = state_value_t(state);

		// just give them names
		auto & a = wvar[0];
		auto & b = wvar[1];
		auto & c = wvar[2];
		auto & d = wvar[3];
		auto & e = wvar[4];
		auto & f = wvar[5];
		auto & g = wvar[6];
		auto & h = wvar[7];

		for (int i = 0; i != config.rounds_number; ++i) {
			const state_item_t S1 = std::rotr(e, config.compress_constants[0]) xor std::rotr(e, config.compress_constants[1]) xor std::rotr(e, config.compress_constants[2]);
			const state_item_t choice = (e bitand f) xor (~e bitand g);
			const state_item_t temp1 = h + S1 + choice + config.constants[i] + w[i];

			const state_item_t S0 = std::rotr(a, config.compress_constants[3]) xor std::rotr(a, config.compress_constants[4]) xor std::rotr(a, config.compress_constants[5]);
			const state_item_t majority = (a bitand b) xor (a bitand c) xor (b bitand c);
			const state_item_t temp2 = S0 + majority;

			// move around
			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}

		// add store back
		for (int i = 0; i != (int)state.size(); ++i) {
			// std::cout << "h" << i << " = " << std::bitset<32>(hash[i]) << " + " << std::bitset<32>(wvar[i]) << " = " << std::bitset<32>(hash[i] + wvar[i]) << "\n";
			state[i] += wvar[i];
		}
	}

	// this implementation works only with input size aligned to bytes (not bits)
	template <byte_like T> constexpr void update_to_buffer_and_process(std::span<const T> in) noexcept {
		for (;;) {
			const auto remaining_free_space = std::span<std::byte, block_size_bytes>(block).subspan(block_used);
			const auto to_copy = in.first(std::min(in.size(), remaining_free_space.size()));

			const auto it = byte_copy(to_copy.begin(), to_copy.end(), remaining_free_space.begin());

			total_length += to_copy.size();

			if (it != remaining_free_space.end()) {
				block_used += to_copy.size();
				return;
			} else {
				block_used = 0zu;
			}

			assert(it == remaining_free_space.end());

			// we have block!
			const staging_value_t w = build_staging(block);
			rounds(w, hash);

			// continue with the next block (if there is any)
			in = in.subspan(to_copy.size());
			// TODO maybe avoid copying the data and process it directly over span
		}

		return;
	}

	constexpr void finalize_buffer() noexcept {
		// we know it's not used completely, otherwise `update_to_buffer_and_process` function would process it
		assert(block_used < block.size());
		const auto free_space = std::span(block).subspan(block_used);

		auto it = free_space.data();
		*it++ = std::byte{0b1000'0000u};							   // first byte after data contains bit at MSB
		std::fill(it, (block.data() + block.size()), std::byte{0x0u}); // rest is filled with zeros

		if (free_space.size() < (1zu + (config.length_size_bits / 8zu))) {
			// process block without length at the end
			const staging_value_t w = build_staging(block);
			rounds(w, hash);

			// create block of all zeros
			std::fill(block.begin(), block.end(), std::byte{0x0u});
		}

		// add total_length at the end of block (in bits)
		// this works even if we have uint128_t for size
		unwrap_bigendian_number{std::span(block).template last<sizeof(length_t)>()} = (total_length * 8zu);

		const staging_value_t w = build_staging(block);
		rounds(w, hash);
	}

	constexpr void write_result_into(digest_span_t out) noexcept {
		// copy result to byte result
		static_assert(config.values_for_output <= config.initial_values.size());

		for (int i = 0; i != config.values_for_output; ++i) {
			unwrap_bigendian_number<state_item_t>{out.subspan(i * sizeof(state_item_t)).template first<sizeof(state_item_t)>()} = hash[i];
		}
	}
};

// this is a convinience type for nicer UX...
template <typename Config> struct hasher: private internal_hasher<Config> {
	using super = internal_hasher<Config>;
	using result_t = typename super::result_t;
	using length_t = typename super::length_t;
	using digest_span_t = typename super::digest_span_t;

	constexpr hasher() noexcept: super() { }
	constexpr hasher(const hasher &) noexcept = default;
	constexpr hasher(hasher &&) noexcept = default;
	constexpr ~hasher() noexcept = default;

	// support for various input types
	constexpr hasher & update(std::span<const std::byte> input) noexcept {
		super::update_to_buffer_and_process(input);
		return *this;
	}

	template <convertible_to_byte_span T> constexpr hasher & update(const T & something) noexcept {
		using value_type = typename decltype(std::span(something))::value_type;
		super::update_to_buffer_and_process(std::span<const value_type>(something));
		return *this;
	}

	template <one_byte_char CharT> constexpr hasher & update(std::basic_string_view<CharT> in) noexcept {
		super::update_to_buffer_and_process(std::span(in.data(), in.size()));
		return *this;
	}

	template <string_literal T> constexpr hasher & update(const T & lit) noexcept {
		super::update_to_buffer_and_process(std::span(lit, std::size(lit) - 1zu));
		return *this;
	}

	// output (by reference or by value)
	constexpr void final(digest_span_t digest) noexcept {
		super::finalize_buffer();
		super::write_result_into(digest);
	}

	constexpr auto final() noexcept {
		result_t output;
		this->final(output);
		return output;
	}

	constexpr length_t size() const noexcept {
		return super::total_length;
	}
};

} // namespace cthash

#endif
