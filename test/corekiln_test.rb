# frozen_string_literal: true

require "fileutils"
require "minitest/autorun"
require "open3"
require "timeout"
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

  def test_rejects_non_positive_and_non_numeric_values
    with_compiled_corekiln do |binary|
      [
        [%w[--workers 0], "--workers requires a positive integer"],
        [%w[--workers many], "--workers requires a positive integer"],
        [%w[--duration 0], "--duration requires a positive integer"],
        [%w[--duration forever], "--duration requires a positive integer"],
      ].each do |arguments, expected_error|
        _stdout, stderr, status = Open3.capture3(binary, *arguments)

        refute status.success?, "Expected #{arguments.inspect} to fail"
        assert_includes stderr, expected_error
      end
    end
  end

  def test_rejects_missing_values_and_unknown_options
    with_compiled_corekiln do |binary|
      [
        [%w[--workers], "--workers requires a positive integer"],
        [%w[--duration], "--duration requires a positive integer"],
        [%w[--unknown], "unknown option: --unknown"],
      ].each do |arguments, expected_error|
        _stdout, stderr, status = Open3.capture3(binary, *arguments)

        refute status.success?, "Expected #{arguments.inspect} to fail"
        assert_includes stderr, expected_error
      end
    end
  end

  def test_runs_requested_workers_for_the_duration
    with_compiled_corekiln do |binary|
      started_at = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      stdout, stderr, status = Open3.capture3(
        binary,
        "--workers",
        "2",
        "--duration",
        "1",
      )
      elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started_at

      assert status.success?, stderr
      assert_operator elapsed, :>=, 0.8
      assert_operator elapsed, :<, 3.0
      assert_includes stdout, "corekiln: burning 2 workers for 1 second"
      assert_includes stdout, "corekiln: stopped"
      assert_empty stderr
    end
  end

  def test_interrupt_stops_an_unbounded_run
    with_compiled_corekiln do |binary|
      Open3.popen3(binary, "--workers", "1") do |stdin, stdout, stderr, wait|
        stdin.close
        begin
          startup = Timeout.timeout(5) { stdout.gets }

          assert_equal "corekiln: burning 1 worker until interrupted\n", startup

          Process.kill("INT", wait.pid)
          Timeout.timeout(5) { wait.join }

          assert wait.value.success?, stderr.read
          assert_includes stdout.read, "corekiln: stopped"
        ensure
          if wait.alive?
            begin
              Process.kill("TERM", wait.pid)
            rescue Errno::ESRCH
              nil
            end
            wait.join(2)
          end
        end
      end
    end
  end

  def test_wrapper_builds_and_forwards_arguments
    Dir.mktmpdir("corekiln-wrapper-test") do |directory|
      source_wrapper = File.join(ROOT, "bin/corekiln")
      assert File.file?(source_wrapper), "Expected bin/corekiln to exist"

      FileUtils.mkdir_p(File.join(directory, "bin"))
      FileUtils.mkdir_p(File.join(directory, "src"))
      FileUtils.cp(source_wrapper, File.join(directory, "bin"))
      FileUtils.cp(SOURCE, File.join(directory, "src"))
      wrapper = File.join(directory, "bin/corekiln")
      FileUtils.chmod(0o755, wrapper)

      stdout, stderr, status = Open3.capture3(
        wrapper,
        "--workers",
        "1",
        "--duration",
        "1",
      )

      assert status.success?, stderr
      assert File.executable?(File.join(directory, ".build/corekiln"))
      assert_includes stdout, "corekiln: burning 1 worker for 1 second"
      assert_includes stdout, "corekiln: stopped"
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
