// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <wordexp.h>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "cc/check.h"
#include "cc/constants.h"
#include "cc/dual_net/batching_dual_net.h"
#include "cc/dual_net/factory.h"
#include "cc/file/path.h"
#include "cc/file/utils.h"
#include "cc/gtp_player.h"
#include "cc/init.h"
#include "cc/mcts_player.h"
#include "cc/random.h"
#include "cc/sgf.h"
#include "cc/tf_utils.h"
#include "cc/zobrist.h"
#include "gflags/gflags.h"

// Game options flags.
DEFINE_string(mode, "",
              "Mode to run in: \"selfplay\", \"eval\", \"gtp\" or \"puzzle\".");
DEFINE_int32(
    ponder_limit, 0,
    "If non-zero and in GTP mode, the number times of times to perform tree "
    "search while waiting for the opponent to play.");
DEFINE_bool(
    courtesy_pass, false,
    "If true and in GTP mode, we will always pass if the opponent passes.");
DEFINE_double(resign_threshold, -0.999, "Resign threshold.");
DEFINE_double(komi, minigo::kDefaultKomi, "Komi.");
DEFINE_double(disable_resign_pct, 0.1,
              "Fraction of games to disable resignation for.");
DEFINE_uint64(seed, 0,
              "Random seed. Use default value of 0 to use a time-based seed. "
              "This seed is used to control the moves played, not whether a "
              "game has resignation disabled or is a holdout.");

// Tree search flags.
DEFINE_int32(num_readouts, 100,
             "Number of readouts to make during tree search for each move.");
DEFINE_int32(virtual_losses, 8,
             "Number of virtual losses when running tree search.");
DEFINE_bool(inject_noise, true,
            "If true, inject noise into the root position at the start of "
            "each tree search.");
DEFINE_bool(soft_pick, true,
            "If true, choose moves early in the game with a probability "
            "proportional to the number of times visited during tree search. "
            "If false, always play the best move.");
DEFINE_bool(random_symmetry, true,
            "If true, randomly flip & rotate the board features before running "
            "the model and apply the inverse transform to the results.");
DEFINE_string(flags_path, "",
              "Optional path to load flags from. Flags specified in this file "
              "take priority over command line flags. When running selfplay "
              "with run_forever=true, the flag file is reloaded periodically. "
              "Note that flags_path is different from gflags flagfile, which "
              "is only parsed once on startup.");

// Time control flags.
DEFINE_double(seconds_per_move, 0,
              "If non-zero, the number of seconds to spend thinking about each "
              "move instead of using a fixed number of readouts.");
DEFINE_double(
    time_limit, 0,
    "If non-zero, the maximum amount of time to spend thinking in a game: we "
    "spend seconds_per_move thinking for each move for as many moves as "
    "possible before exponentially decaying the amount of time.");
DEFINE_double(decay_factor, 0.98,
              "If time_limit is non-zero, the decay factor used to shorten the "
              "amount of time spent thinking as the game progresses.");
DEFINE_bool(run_forever, false,
            "When running 'selfplay' mode, whether to run forever.");

// Inference flags.
DEFINE_string(model, "",
              "Path to a minigo model. The format of the model depends on the "
              "inferece engine. For engine=tf, the model should be a GraphDef "
              "proto. For engine=lite, the model should be .tflite "
              "flatbuffer. For engine=trt, the model should be a .uff graph.");
DEFINE_string(model_two, "",
              "When running 'eval' mode, provide a path to a second minigo "
              "model, also serialized as a GraphDef proto. Exactly one of "
              "model_two and gtp_client needs to be specified in eval mode.");
DEFINE_string(gtp_client, "",
              "When running 'eval' mode, provide a path and arguments to an "
              "executable which accepts GTP commands on stdin. Example: "
              "'/usr/games/gnugo --mode gtp'. Exactly one of model_two and "
              "gtp_client needs to be specified in eval mode.");
DEFINE_int32(parallel_games, 32, "Number of games to play in parallel.");

// Output flags.
DEFINE_string(output_dir, "",
              "Output directory. If empty, no examples are written.");
DEFINE_string(holdout_dir, "",
              "Holdout directory. If empty, no examples are written.");
DEFINE_string(output_bigtable, "",
              "Output Bigtable specification, of the form: "
              "project,instance,table. "
              "If empty, no examples are written to Bigtable.");
DEFINE_string(sgf_dir, "",
              "SGF directory for selfplay and puzzles. If empty in selfplay "
              "mode, no SGF is written.");
DEFINE_double(holdout_pct, 0.03,
              "Fraction of games to hold out for validation.");

// Self play flags:
//   --inject_noise=true
//   --soft_pick=true
//   --random_symmetery=true
//
// Two player flags:
//   --inject_noise=false
//   --soft_pick=false
//   --random_symmetry=true

namespace minigo {
namespace {

std::unique_ptr<DualNetFactory> NewDualNetFactory(const std::string& model_path,
                                                  int num_parallel_games) {
  auto dual_net = NewDualNet(model_path);
  // Calculate batch size suiteable for a DualNet which handles inference
  // requests from num_parallel_games each with at most virtual_losses features
  // each so that the maximum number of features in flight results in
  // buffer_count batches.
  int buffer_count = dual_net->GetBufferCount();
  size_t batch_size =
      std::max((FLAGS_virtual_losses * num_parallel_games + buffer_count - 1) /
                   buffer_count,
               FLAGS_virtual_losses);
  return NewBatchingFactory(std::move(dual_net), batch_size);
}

std::string GetOutputName(absl::Time now, size_t i) {
  auto timestamp = absl::ToUnixSeconds(now);
  std::string output_name;
  char hostname[64];
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    std::strncpy(hostname, "unknown", sizeof(hostname));
  }
  return absl::StrCat(timestamp, "-", hostname, "-", i);
}

std::string GetOutputDir(absl::Time now, const std::string& root_dir) {
  auto sub_dirs = absl::FormatTime("%Y-%m-%d-%H", now, absl::UTCTimeZone());
  return file::JoinPath(root_dir, sub_dirs);
}

std::string FormatInferenceInfo(
    const std::vector<MctsPlayer::InferenceInfo>& inferences) {
  std::vector<std::string> parts;
  parts.reserve(inferences.size());
  for (const auto& info : inferences) {
    parts.push_back(absl::StrCat(info.model, "(", info.first_move, ",",
                                 info.last_move, ")"));
  }
  return absl::StrJoin(parts, ", ");
}

void WriteSgf(const std::string& output_dir, const std::string& output_name,
              const MctsPlayer& player_b, const std::string& name_b,
              const MctsPlayer& player_w, const std::string& name_w,
              bool write_comments) {
  MG_CHECK(file::RecursivelyCreateDir(output_dir));
  MG_CHECK(player_b.history().size() == player_w.history().size());

  bool log_names = name_b != name_w;

  std::vector<sgf::MoveWithComment> moves;
  moves.reserve(player_b.history().size());

  for (size_t i = 0; i < player_b.history().size(); ++i) {
    const auto& h = i % 2 == 0 ? player_b.history()[i] : player_w.history()[i];
    const auto& color = h.node->position.to_play();
    std::string comment;
    if (write_comments) {
      if (i == 0) {
        comment = absl::StrCat(
            "Resign Threshold: ", player_b.options().resign_threshold, "\n",
            h.comment);
      } else {
        if (log_names) {
          comment = absl::StrCat(i % 2 == 0 ? name_b : name_w, "\n", h.comment);
        } else {
          comment = h.comment;
        }
      }
      moves.emplace_back(color, h.c, std::move(comment));
    } else {
      moves.emplace_back(color, h.c, "");
    }
  }

  sgf::CreateSgfOptions options;
  options.komi = player_b.options().komi;
  options.result = player_b.result_string();
  options.black_name = name_b;
  options.white_name = name_w;
  options.game_comment = absl::StrCat(
      "B inferences: ", FormatInferenceInfo(player_b.inferences()), "\n",
      "W inferences: ", FormatInferenceInfo(player_w.inferences()));

  auto sgf_str = sgf::CreateSgfString(moves, options);

  auto output_path = file::JoinPath(output_dir, output_name + ".sgf");
  MG_CHECK(file::WriteFile(output_path, sgf_str));
}

void WriteSgf(const std::string& output_dir, const std::string& output_name,
              const MctsPlayer& player_b, const MctsPlayer& player_w,
              bool write_comments) {
  WriteSgf(output_dir, output_name, player_b, player_b.name(), player_w,
           player_w.name(), write_comments);
}

void WriteSgf(const std::string& output_dir, const std::string& output_name,
              const MctsPlayer& player, bool write_comments) {
  WriteSgf(output_dir, output_name, player, player, write_comments);
}

struct EvalResults {
  EvalResults(absl::string_view _name)
      : name(_name), black_wins(0), white_wins(0) {}
  std::string name;
  std::atomic<int> black_wins;
  std::atomic<int> white_wins;
};

void LogEvalResults(int num_games, const EvalResults& results_a,
                    const EvalResults& results_b) {
  auto name_length = std::max(results_a.name.size(), results_b.name.size());
  auto format_name = [&](const std::string& name) {
    return absl::StrFormat("%-*s", name_length, name);
  };
  auto format_wins = [&](int wins) {
    return absl::StrFormat(" %5d %6.2f%%", wins, wins * 100.0f / num_games);
  };
  auto print_result = [&](const EvalResults& results) {
    std::cerr << format_name(results.name)
              << format_wins(results.black_wins + results.white_wins)
              << format_wins(results.black_wins)
              << format_wins(results.white_wins) << std::endl;
  };

  std::cerr << format_name("Wins")
            << "        Total         Black         White" << std::endl;
  print_result(results_a);
  print_result(results_b);
  std::cerr << format_name("") << "              "
            << format_wins(results_a.black_wins + results_b.black_wins)
            << format_wins(results_a.white_wins + results_b.white_wins);
  std::cerr << std::endl;
}

void ParseMctsPlayerOptionsFromFlags(MctsPlayer::Options* options) {
  options->inject_noise = FLAGS_inject_noise;
  options->soft_pick = FLAGS_soft_pick;
  options->random_symmetry = FLAGS_random_symmetry;
  options->resign_threshold = FLAGS_resign_threshold;
  options->batch_size = FLAGS_virtual_losses;
  options->komi = FLAGS_komi;
  options->random_seed = FLAGS_seed;
  options->num_readouts = FLAGS_num_readouts;
  options->seconds_per_move = FLAGS_seconds_per_move;
  options->time_limit = FLAGS_time_limit;
  options->decay_factor = FLAGS_decay_factor;
}

void LogEndGameInfo(const MctsPlayer& player, absl::Duration game_time) {
  std::cout << player.result_string() << std::endl;
  std::cout << "Playing game: " << absl::ToDoubleSeconds(game_time)
            << std::endl;
  std::cout << "Played moves: " << player.root()->position.n() << std::endl;

  const auto& history = player.history();
  if (history.empty()) {
    return;
  }

  int bleakest_move = 0;
  float q = 0.0;
  if (FindBleakestMove(player, &bleakest_move, &q)) {
    std::cout << "Bleakest eval: move=" << bleakest_move << " Q=" << q
              << std::endl;
  }

  // If resignation is disabled, check to see if the first time Q_perspective
  // crossed the resign_threshold the eventual winner of the game would have
  // resigned. Note that we only check for the first resignation: if the
  // winner would have incorrectly resigned AFTER the loser would have
  // resigned on an earlier move, this is not counted as a bad resignation for
  // the winner (since the game would have ended after the loser's initial
  // resignation).
  float result = player.result();
  if (!player.options().resign_enabled) {
    for (size_t i = 0; i < history.size(); ++i) {
      if (history[i].node->Q_perspective() <
          player.options().resign_threshold) {
        if ((history[i].node->Q() < 0) != (result < 0)) {
          std::cout << "Bad resign: move=" << i << " Q=" << history[i].node->Q()
                    << std::endl;
        }
        break;
      }
    }
  }
}

class SelfPlayer {
 public:
  void Run() {
    auto start_time = absl::Now();
    {
      absl::MutexLock lock(&mutex_);
      dual_net_factory_ = NewDualNetFactory(FLAGS_model, FLAGS_parallel_games);
    }
    for (int i = 0; i < FLAGS_parallel_games; ++i) {
      threads_.emplace_back(std::bind(&SelfPlayer::ThreadRun, this, i));
    }
    for (auto& t : threads_) {
      t.join();
    }
    std::cerr << "Played " << FLAGS_parallel_games << " games, total time "
              << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec."
              << std::endl;
  }

 private:
  // Struct that holds the options for a game. Each thread has its own
  // GameOptions instance, which are initialized with the SelfPlayer's mutex
  // held. This allows us to safely update the command line arguments from a
  // flag file without causing any race conditions.
  struct GameOptions {
    void Init(int thread_id, Random* rnd) {
      ParseMctsPlayerOptionsFromFlags(&player_options);
      player_options.verbose = thread_id == 0;
      // If an random seed was explicitly specified, make sure we use a
      // different seed for each thread.
      if (player_options.random_seed != 0) {
        player_options.random_seed += 1299283 * thread_id;
      }
      player_options.resign_enabled = (*rnd)() >= FLAGS_disable_resign_pct;

      run_forever = FLAGS_run_forever;
      holdout_pct = FLAGS_holdout_pct;
      output_dir = FLAGS_output_dir;
      holdout_dir = FLAGS_holdout_dir;
      sgf_dir = FLAGS_sgf_dir;
    }

    MctsPlayer::Options player_options;
    bool run_forever;
    float holdout_pct;
    std::string output_dir;
    std::string holdout_dir;
    std::string sgf_dir;
  };

  void ThreadRun(int thread_id) {
    // Only print the board using ANSI colors if stderr is sent to the
    // terminal.
    const bool use_ansi_colors = isatty(fileno(stderr));

    GameOptions game_options;
    std::vector<std::string> bigtable_spec =
        absl::StrSplit(FLAGS_output_bigtable, ',');
    bool use_bigtable = bigtable_spec.size() == 3;
    if (!FLAGS_output_bigtable.empty() && !use_bigtable) {
      MG_FATAL()
          << "Bigtable output must be of the form: project,instance,table";
      return;
    }

    do {
      std::unique_ptr<MctsPlayer> player;

      {
        absl::MutexLock lock(&mutex_);
        auto old_model = FLAGS_model;
        MaybeReloadFlags();
        MG_CHECK(old_model == FLAGS_model)
            << "Manually changing the model during selfplay is not supported.";
        game_options.Init(thread_id, &rnd_);
        player = absl::make_unique<MctsPlayer>(dual_net_factory_->New(),
                                               game_options.player_options);
      }

      // Play the game.
      auto start_time = absl::Now();
      while (!player->root()->game_over()) {
        auto move = player->SuggestMove();
        if (player->options().verbose) {
          const auto& position = player->root()->position;
          std::cerr << player->root()->position.ToPrettyString(use_ansi_colors);
          std::cerr << "Move: " << position.n()
                    << " Captures X: " << position.num_captures()[0]
                    << " O: " << position.num_captures()[1] << std::endl;
          std::cerr << player->root()->Describe() << std::endl;
        }
        player->PlayMove(move);
      }

      {
        // Log the end game info with the shared mutex held to prevent the
        // outputs from multiple threads being interleaved.
        absl::MutexLock lock(&mutex_);
        LogEndGameInfo(*player, absl::Now() - start_time);
      }

      // Write the outputs.
      auto now = absl::Now();
      auto output_name = GetOutputName(now, thread_id);

      bool is_holdout;
      {
        absl::MutexLock lock(&mutex_);
        is_holdout = rnd_() < game_options.holdout_pct;
      }
      auto example_dir =
          is_holdout ? game_options.holdout_dir : game_options.output_dir;
      if (!example_dir.empty()) {
        tf_utils::WriteGameExamples(GetOutputDir(now, example_dir), output_name,
                                    *player);
      }
      if (use_bigtable) {
        const auto& gcp_project_name = bigtable_spec[0];
        const auto& instance_name = bigtable_spec[1];
        const auto& table_name = bigtable_spec[2];
        tf_utils::WriteGameExamples(gcp_project_name, instance_name, table_name,
                                    *player);
      }

      if (!game_options.sgf_dir.empty()) {
        WriteSgf(
            GetOutputDir(now, file::JoinPath(game_options.sgf_dir, "clean")),
            output_name, *player, false);
        WriteSgf(
            GetOutputDir(now, file::JoinPath(game_options.sgf_dir, "full")),
            output_name, *player, true);
      }
    } while (game_options.run_forever);

    std::cerr << "Thread " << thread_id << " stopping" << std::endl;
  }

  void MaybeReloadFlags() EXCLUSIVE_LOCKS_REQUIRED(&mutex_) {
    if (FLAGS_flags_path.empty()) {
      return;
    }
    uint64_t new_flags_timestamp;
    MG_CHECK(file::GetModTime(FLAGS_flags_path, &new_flags_timestamp));
    std::cerr << "flagfile:" << FLAGS_flags_path
              << " old_ts:" << absl::FromUnixMicros(flags_timestamp_)
              << " new_ts:" << absl::FromUnixMicros(new_flags_timestamp);
    if (new_flags_timestamp == flags_timestamp_) {
      std::cerr << " skipping" << std::endl;
      return;
    }

    flags_timestamp_ = new_flags_timestamp;
    std::string contents;
    MG_CHECK(file::ReadFile(FLAGS_flags_path, &contents));

    std::vector<std::string> lines =
        absl::StrSplit(contents, '\n', absl::SkipEmpty());
    std::cerr << " loaded flags:" << absl::StrJoin(lines, " ") << std::endl;

    for (absl::string_view line : lines) {
      std::pair<absl::string_view, absl::string_view> line_comment =
          absl::StrSplit(line, absl::MaxSplits('#', 1));
      line = absl::StripAsciiWhitespace(line_comment.first);
      if (line.empty()) {
        continue;
      }
      MG_CHECK(line.length() > 2 && line[0] == '-' && line[1] == '-') << line;
      std::pair<std::string, std::string> flag_value =
          absl::StrSplit(line, absl::MaxSplits('=', 1));
      flag_value.first = flag_value.first.substr(2);
      std::cerr << "Setting command line flag: --" << flag_value.first << "="
                << flag_value.second << std::endl;
      gflags::SetCommandLineOption(flag_value.first.c_str(),
                                   flag_value.second.c_str());
    }
  }

  absl::Mutex mutex_;
  std::unique_ptr<DualNetFactory> dual_net_factory_ GUARDED_BY(&mutex_);
  Random rnd_ GUARDED_BY(&mutex_);
  std::vector<std::thread> threads_;
  uint64_t flags_timestamp_ = 0;
};

class PairEvaluator {
  // A barrier that blocks threads until the number of waiting threads reaches
  // the 'count' threshold. This implementation has different semantics than
  // absl::Barrier: it can be reused and allows decrementing the threshold to
  // handle the tail of a work queue where some threads exit early.
  class Barrier {
   public:
    explicit Barrier(size_t count)
        : count_(count), num_waiting_(0), generation_(0) {}

    void Wait() {
      absl::MutexLock lock(&mutex_);
      if (++num_waiting_ == count_) {
        IncrementGeneration();
      } else {
        auto generation = generation_;
        while (generation != generation_) {
          cond_var_.Wait(&mutex_);
        }
      }
    }

    void DecrementCount() {
      absl::MutexLock lock(&mutex_);
      if (num_waiting_ == --count_) {
        IncrementGeneration();
      }
    }

   private:
    void IncrementGeneration() EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
      ++generation_;
      num_waiting_ = 0;
      cond_var_.SignalAll();
    }

    absl::Mutex mutex_;
    absl::CondVar cond_var_;
    size_t count_ GUARDED_BY(&mutex_);
    size_t num_waiting_ GUARDED_BY(&mutex_);
    size_t generation_ GUARDED_BY(&mutex_);
  };

  // References a pointer to an actual DualNet. Allows updating the pointer
  // after the MctsPlayer has been constructed.
  class WrappedDualNet : public DualNet {
   public:
    explicit WrappedDualNet(const std::unique_ptr<DualNet>* dual_net)
        : dual_net_(dual_net) {
      MG_CHECK(dual_net);
    }

   private:
    void RunMany(std::vector<const BoardFeatures*> features,
                 std::vector<Output*> outputs, std::string* model) override {
      dual_net_->get()->RunMany(std::move(features), std::move(outputs), model);
    };

    const std::unique_ptr<DualNet>* const dual_net_;
  };

  struct Model {
    Model(const std::string& model_path)
        : factory(NewDualNetFactory(model_path, FLAGS_parallel_games)),
          results(file::Stem(model_path)) {}
    std::unique_ptr<DualNetFactory> factory;
    EvalResults results;
  };

 public:
  void Run() {
    auto start_time = absl::Now();

    auto prev_model = absl::make_unique<Model>(FLAGS_model);
    auto cur_model = absl::make_unique<Model>(FLAGS_model_two);

    std::cerr << "DualNet factories created from " << FLAGS_model << "\n  and "
              << FLAGS_model_two << " in "
              << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec."
              << std::endl;

    ParseMctsPlayerOptionsFromFlags(&options_);
    options_.inject_noise = false;
    options_.soft_pick = false;
    options_.random_symmetry = true;

    int num_games = FLAGS_parallel_games;
    barrier_ = absl::make_unique<Barrier>(num_games);

    for (int thread_id = 0; thread_id < num_games; ++thread_id) {
      bool swap_models = (thread_id & 1) != 0;
      threads_.emplace_back(std::bind(&PairEvaluator::ThreadRun, this,
                                      thread_id, cur_model.get(),
                                      prev_model.get(), swap_models));
    }

    for (auto& t : threads_) {
      t.join();
    }

    std::cerr << "Evaluated " << num_games << " games, total time "
              << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec."
              << std::endl;

    LogEvalResults(num_games, prev_model->results, cur_model->results);
  }

 private:
  void ThreadRun(int thread_id, Model* model, Model* other_model,
                 bool swap_models) {
    if (swap_models) {
      std::swap(model, other_model);
      // Wait for the barrier so that games with swapped models lag one move
      // behind the other games, and the per-model inferences of all games run
      // in sync.
      barrier_->Wait();
    }

    // The player and other_player reference this pointer.
    std::unique_ptr<DualNet> dual_net;

    auto player_options = options_;
    // If an random seed was explicitly specified, make sure we use a
    // different seed for each thread.
    if (player_options.random_seed != 0) {
      player_options.random_seed += 1299283 * thread_id;
    }

    player_options.verbose = thread_id == 0;
    player_options.name = model->results.name;
    auto player = absl::make_unique<MctsPlayer>(
        absl::make_unique<WrappedDualNet>(&dual_net), player_options);

    player_options.verbose = false;
    player_options.name = other_model->results.name;
    auto other_player = absl::make_unique<MctsPlayer>(
        absl::make_unique<WrappedDualNet>(&dual_net), player_options);

    auto* black = player.get();
    auto* white = other_player.get();

    auto* factory = model->factory.get();
    auto* other_factory = other_model->factory.get();

    while (!player->root()->game_over()) {
      // Create the DualNet for a single move and dispose it again. This
      // is required because a BatchingDualNet instance can prevent the
      // inference queue from being flushed if it's not sending any requests.
      // The number of requests per move can be smaller than num_readouts at the
      // end of a game.
      dual_net = factory->New();
      // Wait for all threads to create their DualNet. This prevents runaway
      // threads from flushing the batching queue prematurely. It actually
      // forces all players to move in lock-step to achieve optimal batching.
      barrier_->Wait();
      auto move = player->SuggestMove();
      dual_net.reset();
      if (player->options().verbose) {
        std::cerr << player->root()->Describe() << "\n";
      }
      player->PlayMove(move);
      other_player->PlayMove(move);
      if (player->options().verbose) {
        std::cerr << player->root()->position.ToPrettyString();
      }
      std::swap(factory, other_factory);
      std::swap(player, other_player);
    }
    // Notify the barrier that this thread is no longer participating.
    barrier_->DecrementCount();

    MG_CHECK(player->result() == other_player->result());
    if (player->result() > 0) {
      ++model->results.black_wins;
    }
    if (player->result() < 0) {
      ++other_model->results.white_wins;
    }

    if (black->options().verbose) {
      std::cerr << black->result_string() << "\n";
      std::cerr << "Black was: " << black->name() << "\n";
    }

    // Write SGF.
    if (!FLAGS_sgf_dir.empty()) {
      std::string output_name =
          absl::StrCat(GetOutputName(absl::Now(), thread_id), "-",
                       black->name(), "-", white->name());
      WriteSgf(FLAGS_sgf_dir, output_name, *black, *white, true);
    }

    std::cerr << "Thread " << thread_id << " stopping" << std::endl;
  }

  MctsPlayer::Options options_;
  std::unique_ptr<Barrier> barrier_;
  std::vector<std::thread> threads_;
};

class GtpEvaluator {
  class GtpClient {
   public:
    GtpClient(char* const cmd_args[], float komi) : color_(Color::kBlack) {
      int in_pipe[2];   // minigo <- gnugo pipe
      int out_pipe[2];  // minigo -> gnugo pipe

      MG_CHECK(pipe(in_pipe) == 0);
      MG_CHECK(pipe(out_pipe) == 0);

      if (auto pid = fork()) {
        MG_CHECK(pid > 0);
      } else {
        MG_CHECK(close(in_pipe[0]) == 0);
        MG_CHECK(close(out_pipe[1]) == 0);

        MG_CHECK(dup2(in_pipe[1], STDOUT_FILENO) >= 0);
        MG_CHECK(dup2(out_pipe[0], STDIN_FILENO) >= 0);

        MG_CHECK(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);
        MG_CHECK(execvp(cmd_args[0], cmd_args) == 0);

        exit(0);
      }

      MG_CHECK(close(in_pipe[1]) == 0);
      MG_CHECK(close(out_pipe[0]) == 0);

      input_ = fdopen(in_pipe[0], "r");
      output_ = fdopen(out_pipe[1], "w");
      MG_CHECK(input_ && output_);

      MG_CHECK(Send(absl::StrFormat("boardsize %d", kN)));
      MG_CHECK(Send("komi " + std::to_string(komi)));
    }

    ~GtpClient() {
      fclose(input_);
      fclose(output_);
    }

    bool Play(const Coord& move) {
      std::ostringstream oss;
      oss << "play " << color_ << " " << move.ToKgs();
      bool success = Send(oss.str()).has_value();
      if (success) {
        color_ = OtherColor(color_);
      }
      return success;
    }

    Coord GenMove() {
      std::ostringstream oss;
      oss << "genmove " << color_;
      auto move = Coord::kInvalid;
      if (auto response = Send(oss.str())) {
        move = Coord::FromKgs(response.value(), true);
      }
      if (move != Coord::kInvalid) {
        color_ = OtherColor(color_);
      }
      return move;
    }

    std::string Name() { return Send("name").value_or("<unknown>"); }

   private:
    absl::optional<std::string> Send(const std::string& msg) {
      // std::cerr << "Sending '" << msg << "'" << std::endl;
      MG_CHECK(fprintf(output_, "%s\n", msg.c_str()) > 0);
      MG_CHECK(fflush(output_) == 0);

      char buffer[100];
      size_t length = sizeof(buffer);
      char* ptr = buffer;

      for (;;) {
        std::string response(buffer, getline(&ptr, &length, input_));
        if (response.empty()) {
          continue;
        }

        auto result = response.front();
        if (result == '?') {
          return absl::nullopt;
        }

        if (result == '=') {
          response.erase(response.begin());
          absl::StripAsciiWhitespace(&response);
          return response;
        }
      }
    }

    Color color_;
    FILE* input_;
    FILE* output_;
  };

 public:
  void Run() {
    auto start_time = absl::Now();

    factory_ =
        NewDualNetFactory(FLAGS_model, std::max(FLAGS_parallel_games / 2, 1));
    std::cerr << "DualNet factory created from " << FLAGS_model << " in "
              << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec."
              << std::endl;

    ParseMctsPlayerOptionsFromFlags(&options_);
    MG_CHECK(wordexp(FLAGS_gtp_client.c_str(), &cmd_words_, 0) == 0);

    EvalResults mcts_results(file::Stem(FLAGS_model));
    EvalResults gtp_results("");

    auto* black_results = &mcts_results;
    auto* white_results = &gtp_results;

    std::vector<std::thread> threads;
    for (int thread_id = 0; thread_id < FLAGS_parallel_games; ++thread_id) {
      threads.emplace_back(std::bind(&GtpEvaluator::ThreadRun, this, thread_id,
                                     black_results, white_results,
                                     &gtp_results == black_results));
      std::swap(black_results, white_results);
    }
    for (auto& thread : threads) {
      thread.join();
    }

    wordfree(&cmd_words_);

    std::cerr << "Evaluated " << FLAGS_parallel_games << " games, total time "
              << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec."
              << std::endl;

    LogEvalResults(FLAGS_parallel_games, gtp_results, mcts_results);
  }

 private:
  void ThreadRun(int thread_id, EvalResults* black_results,
                 EvalResults* white_results, bool gtp_is_black) {
    auto player_options = options_;
    player_options.verbose = thread_id == 0;
    // If an random seed was explicitly specified, make sure we use a
    // different seed for each thread.
    if (player_options.random_seed != 0) {
      player_options.random_seed += 1299283 * thread_id;
    }

    auto mcts_player =
        absl::make_unique<MctsPlayer>(factory_->New(), player_options);
    auto gtp_client =
        absl::make_unique<GtpClient>(cmd_words_.we_wordv, options_.komi);

    if (thread_id == 0) {
      MG_CHECK(!gtp_is_black);
      white_results->name = gtp_client->Name();
    }

    if (gtp_is_black) {
      mcts_player->PlayMove(gtp_client->GenMove());
    }

    while (!mcts_player->root()->game_over()) {
      auto move = mcts_player->SuggestMove();
      if (!gtp_client->Play(move)) {
        move = Coord::kResign;
      }
      mcts_player->PlayMove(move);
      if (mcts_player->root()->game_over()) {
        break;
      }
      mcts_player->PlayMove(gtp_client->GenMove());
    }

    if (mcts_player->result() > 0) {
      ++black_results->black_wins;
    }
    if (mcts_player->result() < 0) {
      ++white_results->white_wins;
    }

    // Write SGF.
    if (!FLAGS_sgf_dir.empty()) {
      std::string output_name =
          absl::StrCat(GetOutputName(absl::Now(), thread_id), "-",
                       black_results->name, "-", white_results->name);
      WriteSgf(FLAGS_sgf_dir, output_name, *mcts_player, black_results->name,
               *mcts_player, white_results->name, true);
    }
  }

  MctsPlayer::Options options_;
  std::unique_ptr<DualNetFactory> factory_;
  wordexp_t cmd_words_;
};

void SelfPlay() {
  SelfPlayer player;
  player.Run();
}

void Eval() {
  MG_CHECK(FLAGS_model_two.empty() ^ FLAGS_gtp_client.empty())
      << "In 'eval' mode, please specify exactly one of 'model_two' and "
         "'gtp_client'.";
  if (FLAGS_model_two.empty()) {
    GtpEvaluator evaluator;
    evaluator.Run();
  } else {
    PairEvaluator evaluator;
    evaluator.Run();
  }
}

void Gtp() {
  GtpPlayer::Options options;
  ParseMctsPlayerOptionsFromFlags(&options);

  options.name = absl::StrCat("minigo-", file::Basename(FLAGS_model));
  options.ponder_limit = FLAGS_ponder_limit;
  options.courtesy_pass = FLAGS_courtesy_pass;
  auto dual_net_factory = NewDualNetFactory(FLAGS_model, 1);
  auto player = absl::make_unique<GtpPlayer>(dual_net_factory->New(), options);
  player->Run();
}

void Puzzle() {
  auto start_time = absl::Now();

  std::vector<std::string> sgf_files;
  MG_CHECK(file::ListDir(FLAGS_sgf_dir, &sgf_files));

  std::vector<std::vector<Move>> games;
  int parallel_games = 0;
  for (const auto& sgf_file : sgf_files) {
    if (!absl::EndsWith(sgf_file, ".sgf")) {
      continue;
    }
    auto path = file::JoinPath(FLAGS_sgf_dir, sgf_file);
    std::string contents;
    MG_CHECK(file::ReadFile(path, &contents));
    sgf::Ast ast;
    MG_CHECK(ast.Parse(contents));
    auto moves = GetMainLineMoves(ast);
    parallel_games += moves.size();
    games.emplace_back(std::move(moves));
  }

  auto factory = NewDualNetFactory(FLAGS_model, parallel_games);
  std::cerr << "DualNet factory created from " << FLAGS_model << " in "
            << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec."
            << std::endl;

  MctsPlayer::Options options;
  ParseMctsPlayerOptionsFromFlags(&options);
  options.verbose = false;

  using Pair = std::pair<std::unique_ptr<MctsPlayer>, Move>;
  std::vector<Pair> puzzles;
  for (const auto& moves : games) {
    std::vector<std::unique_ptr<MctsPlayer>> players(moves.size());
    for (auto& player : players) {
      player.reset(new MctsPlayer(factory->New(), options));
    }
    for (const auto& move : moves) {
      puzzles.emplace_back(std::move(players.back()), move);
      players.pop_back();
      for (auto& player : players) {
        player->PlayMove(move.c);
      }
    }
  }

  std::atomic<size_t> result(0);
  std::vector<std::thread> threads;
  for (auto& puzzle : puzzles) {
    threads.emplace_back(std::bind(
        [&](const Pair& pair) {
          if (pair.first->SuggestMove() == pair.second.c) {
            ++result;
          }
        },
        std::move(puzzle)));
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::cerr << absl::StreamFormat(
                   "Solved %d of %d puzzles (%3.1f%%), total time %f sec.",
                   result, puzzles.size(), result * 100.0f / puzzles.size(),
                   absl::ToDoubleSeconds(absl::Now() - start_time))
            << std::endl;
}

}  // namespace
}  // namespace minigo

int main(int argc, char* argv[]) {
  minigo::Init(&argc, &argv);
  minigo::zobrist::Init(FLAGS_seed * 614944751);

  if (FLAGS_mode == "selfplay") {
    minigo::SelfPlay();
  } else if (FLAGS_mode == "eval") {
    minigo::Eval();
  } else if (FLAGS_mode == "gtp") {
    minigo::Gtp();
  } else if (FLAGS_mode == "puzzle") {
    minigo::Puzzle();
  } else {
    std::cerr << "Unrecognized mode \"" << FLAGS_mode << "\"\n";
    return 1;
  }

  return 0;
}
