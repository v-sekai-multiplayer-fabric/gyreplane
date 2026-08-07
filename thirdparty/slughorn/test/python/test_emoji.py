"""
Tests for slughorn/emoji.hpp — slughorn.emoji submodule.

Unicode 15.1 RGI lookup table; 973 single-codepoint entries.
No Atlas required — pure name/codepoint conversion.
"""

import pytest
import slughorn


def test_emoji_submodule_exists():
	assert hasattr(slughorn, "emoji")

def test_table_size():
	# Docstring in slughorn-python.cpp said 973; actual table is 974 (Unicode update).
	assert slughorn.emoji.table_size() == 974

def test_name_to_codepoint_known():
	assert slughorn.emoji.name_to_codepoint("dragon") == 0x1F409

def test_name_to_codepoint_unknown():
	assert slughorn.emoji.name_to_codepoint("not_a_real_emoji") is None

def test_codepoint_to_name_known():
	assert slughorn.emoji.codepoint_to_name(0x1F409) == "dragon"

def test_codepoint_to_name_unknown():
	assert slughorn.emoji.codepoint_to_name(0x0041) is None  # 'A' — not emoji

def test_round_trip():
	# Every codepoint in the table should round-trip through both directions.
	size = slughorn.emoji.table_size()

	for i in range(size):
		cp = slughorn.emoji.codepoint_at_index(i)
		name = slughorn.emoji.codepoint_to_name(cp)

		assert name is not None, f"codepoint_at_index({i}) = {cp:#x} has no name"
		assert slughorn.emoji.name_to_codepoint(name) == cp, f"round-trip failed for {name!r}"

def test_codepoint_at_index_bounds():
	size = slughorn.emoji.table_size()
	# First and last entries must return something.
	assert slughorn.emoji.codepoint_at_index(0) > 0
	assert slughorn.emoji.codepoint_at_index(size - 1) > 0

def test_strip_colons():
	assert slughorn.emoji.strip_colons(":dragon:") == "dragon"
	assert slughorn.emoji.strip_colons("dragon") == "dragon"
	assert slughorn.emoji.strip_colons(":smile:") == "smile"

def test_slack_name_to_codepoint_with_colons():
	assert slughorn.emoji.slack_name_to_codepoint(":dragon:") == 0x1F409

def test_slack_name_to_codepoint_without_colons():
	assert slughorn.emoji.slack_name_to_codepoint("dragon") == 0x1F409

def test_random_codepoint():
	cp = slughorn.emoji.random_codepoint()
	# Must be a valid entry — codepoint_to_name should know it.
	assert slughorn.emoji.codepoint_to_name(cp) is not None
