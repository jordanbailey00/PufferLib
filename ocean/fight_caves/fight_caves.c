/*
 * fight_caves.c — Standalone Fight Caves manual play, CPU replay and inspection.
 *
 * Compiled with: ./build.sh fight_caves --cpu (add --debug for sanitizers).
 * --benchmark retains the optional random-action, headless smoke test.
 */

#include "fight_caves.h"
#include "puffercpu.c"  /* Upstream 5.0 inference only; not its generic viewer main. */
#include <limits.h>
#include <stdio.h>
#include <time.h>

/* H is 8-aligned so FP32 and BF16 native tensor layouts have the same padding.
 * Raw files carry no architecture/version metadata: this checks structure only. */
static int fc_policy_weight_count(int hidden, int layers) {
    double count = (double)hidden * (OBS_SIZE + FC_PUFFER_MASK_SIZE + 1)
                 + 3.0 * hidden * hidden * layers;
    if (hidden <= 0 || hidden % 8 || layers <= 0 || count > INT_MAX - 7)
        return 0;
    return (int)count;
}

static Weights* fc_read_checkpoint(const char* path, int count) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "checkpoint rejected: cannot open %s\n", path);
        return NULL;
    }
    long bytes = -1;
    if (fseek(fp, 0, SEEK_END) == 0) bytes = ftell(fp);
    if (bytes < 0 || (size_t)bytes != (size_t)count * sizeof(float) ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "checkpoint rejected: %s: expected %zu bytes, got %ld\n",
                path, (size_t)count * sizeof(float), bytes);
        fclose(fp);
        return NULL;
    }
    Weights* weights = calloc(1, sizeof(*weights) + ((size_t)count + 7) * sizeof(float));
    if (!weights) {
        fprintf(stderr, "checkpoint rejected: cannot allocate policy weights\n");
        fclose(fp);
        return NULL;
    }
    weights->data = (float*)(weights + 1);
    weights->size = count + 7;
    int valid = fread(weights->data, sizeof(float), count, fp) == (size_t)count;
    valid = valid && fgetc(fp) == EOF && !ferror(fp);
    fclose(fp);
    for (int i = 0; valid && i < count; i++)
        valid = isfinite(weights->data[i]);
    if (!valid) {
        fprintf(stderr, "checkpoint rejected: incomplete or non-finite weights: %s\n", path);
        free(weights);
        return NULL;
    }
    return weights;
}

static int replay(int argc, char** argv, int check_only) {
    if (argc < 3 || argv[2][0] == '-' || strcmp(argv[2], "latest") == 0) {
        fprintf(stderr, "Usage: %s %s CHECKPOINT.bin [--headless] "
                "[--section.key=value ...]\nCPU replay requires an explicit checkpoint path.\n",
                argv[0], check_only ? "check" : "eval");
        return 2;
    }
    int headless = 0, ini_argc = 0;
    char* ini_argv[argc];
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) headless = 1;
        else ini_argv[ini_argc++] = argv[i];
    }
    Ini ini = {0};
    puf_ini_load_env(&ini, "fight_caves", ini_argc, ini_argv);
    double h = puf_ini_get(&ini, "policy", "hidden_size");
    double l = puf_ini_get(&ini, "policy", "num_layers");
    double limit = puf_ini_get(&ini, "base", "eval_episodes");
    double seed = puf_ini_get(&ini, "base", "seed");
    if (!isfinite(h) || !isfinite(l) || h < 1 || h > INT_MAX || l < 1 ||
        l > INT_MAX || h != floor(h) || l != floor(l) ||
        !isfinite(limit) || limit < (headless ? 1 : 0) || limit > INT_MAX ||
        limit != floor(limit) || !isfinite(seed) || seed < 0 ||
        seed > UINT_MAX || seed != floor(seed)) {
        fprintf(stderr, "invalid CPU replay configuration: positive integer policy dimensions, "
                "nonnegative integer seed and episode limit required "
                "(headless needs at least one episode)\n");
        puf_ini_free(&ini);
        return 2;
    }
    int hidden = (int)h, layers = (int)l;
    int count = fc_policy_weight_count(hidden, layers);
    if (!count) {
        fprintf(stderr, "unsupported CPU policy shape: hidden size must be a multiple of 8 "
                "and the weight count must fit native indexing\n");
        puf_ini_free(&ini);
        return 2;
    }
    Weights* weights = fc_read_checkpoint(argv[2], count);
    if (!weights) {
        puf_ini_free(&ini);
        return 1;
    }
    printf("CPU_META env=fight_caves path=%s file_floats=%d hidden=%d layers=%d\n",
           argv[2], count, hidden, layers);
    if (check_only) {
        free(weights);
        puf_ini_free(&ini);
        return 0;
    }
    int act_sizes[] = ACT_SIZES;
    PufferNet* net = make_puffernet(weights, 1, OBS_SIZE, hidden, layers,
                                  act_sizes, NUM_ATNS);
    Env env = {0};
    float obs[OBS_SIZE] = {0}, actions[NUM_ATNS] = {0}, reward = 0, terminal = 0;
    unsigned char mask[FC_PUFFER_MASK_SIZE] = {0};
    puf_init(&env, puf_ini_section(&ini, "env", 0));
    env.rng = (unsigned)seed;
    srand((unsigned)seed);
    env.agents[0].observations = obs;
    env.agents[0].actions = actions;
    env.agents[0].rewards = &reward;
    env.agents[0].terminals = &terminal;
    env.agents[0].action_mask = mask;
    puf_reset(&env);
    long steps = 0;
    int episodes = 0;
    /* puf_render owns pacing, pause and single-step; never step its state copy. */
    if (!headless) puf_render(&env);
    while (!limit || episodes < (int)limit) {
        forward_puffernet(net, obs, actions, mask, &terminal);
        puf_step(&env);
        steps++;
        if (terminal > 0.5f) episodes++;
        if (!headless) puf_render(&env);
    }
    float score = env.log.n ? env.log.jad_kill_rate / env.log.n : 0;
    printf("CPU_EVAL env=fight_caves score=%.6f perf=%.6f games=%d steps=%ld params=%d\n",
           score, score, episodes, steps, count);
    puf_close(&env);
    free_puffernet(net);
    free(weights);
    puf_ini_free(&ini);
    return 0;
}

static int benchmark(void) {
    FightCaves env = {0};
    env.num_agents = 1;
    env.agents[0].observations = (float*)calloc(FC_PUFFER_OBS_SIZE, sizeof(float));
    env.agents[0].actions = (float*)calloc(FC_PUFFER_NUM_ATNS, sizeof(float));
    env.agents[0].rewards = (float*)calloc(1, sizeof(float));
    env.agents[0].terminals = (float*)calloc(1, sizeof(float));

    {
        env.reward_params = fc_reward_default_params();
        env.initial_sharks = 0;
        env.initial_prayer_doses = 0;

        /* Obs ablation flags default to 0 (no ablation) for the standalone harness. */
        env.obs_ablate_npc_distance = 0;
        env.obs_ablate_incoming_aggregates = 0;
        env.obs_ablate_npc_valid = 0;
    }

    fc_init(&env.state);

    srand((unsigned)time(NULL));
    int episodes = 100;
    int total_ticks = 0;
    float total_reward = 0;
    int max_wave = 0;

    printf("Running %d episodes with random actions...\n", episodes);
    clock_t start = clock();

    for (int ep = 0; ep < episodes; ep++) {
        puf_reset(&env);
        env.agents[0].terminals[0] = 0.0f;
        int ep_ticks = 0;
        while (!env.agents[0].terminals[0] && ep_ticks < 30000) {
            if (env.state.current_wave > max_wave) {
                max_wave = env.state.current_wave;
            }
            for (int h = 0; h < FC_PUFFER_NUM_ATNS; h++)
                env.agents[0].actions[h] = (float)(rand() % 17);
            env.agents[0].actions[0] = (rand() % 3 == 0) ? (float)(rand() % 17) : 0.0f;
            env.agents[0].actions[1] = (rand() % 5 == 0) ? (float)(rand() % 9) : 0.0f;
            env.agents[0].actions[2] = (rand() % 10 == 0)
                ? (float)(rand() % FC_PRAYER_DIM) : 0.0f;
            puf_step(&env);
            total_reward += env.agents[0].rewards[0];
            ep_ticks++;
        }
        total_ticks += ep_ticks;
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Results:\n");
    printf("  Episodes:    %d\n", episodes);
    printf("  Total ticks: %d\n", total_ticks);
    printf("  SPS:         %.0f steps/sec\n", total_ticks / elapsed);
    printf("  Avg reward:  %.2f\n", total_reward / episodes);
    printf("  Max wave:    %d\n", max_wave);
    printf("  Time:        %.2fs\n", elapsed);
    printf("  Log: ep_len=%.1f wave=%.1f n=%.0f\n",
           env.log.episode_length, env.log.wave_reached, env.log.n);

    puf_close(&env);
    free(env.agents[0].observations);
    free(env.agents[0].actions);
    free(env.agents[0].rewards);
    free(env.agents[0].terminals);
    return 0;
}

#ifndef FC_CPU_TEST
int main(int argc, char** argv) {
    if (argc > 1 && (strcmp(argv[1], "eval") == 0 || strcmp(argv[1], "check") == 0))
        return replay(argc, argv, strcmp(argv[1], "check") == 0);
    if (argc == 2 && strcmp(argv[1], "--help") == 0)
        printf("CPU tools: %s --contract | --benchmark | check CHECKPOINT.bin | "
               "eval CHECKPOINT.bin [--headless] [--section.key=value ...]\n", argv[0]);
    /* Inspect this compiled adapter without loading assets or opening a window. */
    if (argc == 2 && strcmp(argv[1], "--contract") == 0) {
        puts(fc_training_contract_json());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0)
        return benchmark();
    return fc_viewer_main(argc, argv);
}
#endif
