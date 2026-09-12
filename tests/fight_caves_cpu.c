/* Native CPU policy plumbing, independent of CUDA and the graphical loop. */
#define FC_CPU_TEST
#include "../ocean/fight_caves/fight_caves.c"

int main(void) {
    assert(fc_policy_weight_count(512, 3) == 2541056);
    assert(fc_policy_weight_count(0, 3) == 0);
    assert(fc_policy_weight_count(15, 3) == 0);
    assert(fc_policy_weight_count(512, 0) == 0);
    assert(fc_policy_weight_count(INT_MAX, INT_MAX) == 0);
    int count = fc_policy_weight_count(8, 2);
    Weights* weights = calloc(1, sizeof(*weights) + (count + 7) * sizeof(float));
    assert(weights);
    weights->data = (float*)(weights + 1);
    weights->size = count + 7;
    int dims[] = ACT_SIZES;
    PufferNet* carried = make_puffernet(weights, 1, OBS_SIZE, 8, 2, dims, NUM_ATNS);
    weights->idx = 0;
    PufferNet* fresh = make_puffernet(weights, 1, OBS_SIZE, 8, 2, dims, NUM_ATNS);
    float obs[OBS_SIZE] = {0}, actions[NUM_ATNS], other[NUM_ATNS];
    unsigned char mask[FC_PUFFER_MASK_SIZE] = {0};
    mask[2] = mask[17 + 3] = mask[17 + 9 + 4] = 1;
    float terminal = 0;
    forward_puffernet(carried, obs, actions, mask, &terminal);
    float once[16];
    memcpy(once, carried->mingru->state, sizeof(once));
    forward_puffernet(carried, obs, actions, mask, &terminal);
    assert(memcmp(once, carried->mingru->state, sizeof(once)) != 0);
    assert(actions[0] == 2 && actions[1] == 3 && actions[2] == 4);
    terminal = 1;
    forward_puffernet(carried, obs, actions, mask, &terminal);
    terminal = 0;
    forward_puffernet(fresh, obs, other, mask, &terminal);
    assert(memcmp(carried->mingru->state, fresh->mingru->state, sizeof(once)) == 0);
    assert(memcmp(actions, other, sizeof(actions)) == 0);
    free_puffernet(carried);
    free_puffernet(fresh);
    free(weights);
    puts("CPU policy: native layout, masks, carry and terminal resets passed");
    return 0;
}
