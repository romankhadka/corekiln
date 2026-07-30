# frozen_string_literal: true

require "fileutils"
require "minitest/autorun"
require "open3"
require "tmpdir"

class CorekilnTest < Minitest::Test
  ROOT = File.expand_path("..", __dir__)
  SOURCE = File.join(ROOT, "src/corekiln.c")
  COMPILER_FLAGS = %w[-std=c11 -O2 -Wall -Wextra -Werror -pthread].freeze

  def test_help_describes_the_command
    with_compiled_corekiln do |binary|
      stdout, stderr, status = Open3.capture3(binary, "--help")

      assert status.success?, stderr
      assert_includes stdout, "Usage: corekiln"
      assert_includes stdout, "--workers N"
      assert_includes stdout, "--duration SECONDS"
      assert_empty stderr
    end
  end

  private

  def with_compiled_corekiln
    Dir.mktmpdir("corekiln-test") do |directory|
      binary = File.join(directory, "corekiln")
      _stdout, stderr, status = Open3.capture3(
        "cc",
        *COMPILER_FLAGS,
        SOURCE,
        "-o",
        binary,
      )
      assert status.success?, "Compilation failed:\n#{stderr}"

      yield binary
    end
  end
end
