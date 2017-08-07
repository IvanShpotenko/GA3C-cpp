#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "gym-uds.h"
#include "model.h"
#include "queue.h"

using experience_t = std::tuple<gym_uds::observation_t, gym_uds::action_t, float, gym_uds::observation_t, bool>;
using memory_t = std::deque<experience_t>;


const uint32_t NUM_AGENTS = 5;
const uint32_t MAX_NUM_TRAINING_EXPERIENCES = 75000;

const uint32_t BATCH_SIZE = 32;

const uint32_t N_STEP_RETURN = 8;
const float GAMMA = 0.99f;

const float EPSILON_START = 1.00f;
const float EPSILON_END   = 0.15f;


Model model;
Queue<experience_t> experiences_queue;
uint32_t num_training_experiences = 0;

std::random_device random_device;
std::default_random_engine random_engine(random_device());

/******************************************************************************/

gym_uds::action_t pick_action(gym_uds::Environment& env, const gym_uds::observation_t& state, float epsilon)
{
    if (std::uniform_real_distribution<float>(0.0f, 1.0f)(random_engine) < epsilon) {
        return env.sample();
    }
    else {
        const auto actions_probs = model.predict_policy(state);
        return std::discrete_distribution<float>(actions_probs.cbegin(), actions_probs.cend())(random_engine);
    }
}

experience_t pick_experience(const memory_t& memory)
{
    const auto n = std::min<uint32_t>(N_STEP_RETURN, memory.size());

    const auto curr_state = std::get<0>(memory[0]);
    const auto action = std::get<1>(memory[0]);
    float reward = 0.0f;
    for (auto i = 0; i < n; ++i) {
        const auto reward_i = std::get<2>(memory[i]);
        reward += std::pow(GAMMA, i)*reward_i;
    }

    const auto next_state = std::get<3>(memory[n-1]);
    const auto done = std::get<4>(memory[n-1]);
    if (not done) {
        reward += std::pow(GAMMA, n)*model.predict_reward(next_state);
    }

    return {curr_state, action, reward, next_state, done};
}

void agent(uint32_t i)
{
    auto env = gym_uds::Environment("/tmp/gym-uds-socket-GA3C-" + std::to_string(i));

    memory_t memory;
    for (uint32_t episode = 1; true; ++episode) {
        gym_uds::observation_t curr_state, next_state;
        curr_state = env.reset();

        const float t = (float)num_training_experiences / (MAX_NUM_TRAINING_EXPERIENCES-1);
        const float epsilon = (1-t)*EPSILON_START + t*EPSILON_END;

        float reward = 0.0f;
        float episode_reward = 0.0f;

        bool done = false;
        while (not done) {
            if (num_training_experiences >= MAX_NUM_TRAINING_EXPERIENCES) { return; }

            const auto action = pick_action(env, curr_state, epsilon);
            std::tie(next_state, reward, done) = env.step(action);
            episode_reward += reward;

            experience_t experience = {curr_state, action, reward, next_state, done};
            memory.push_back(experience);

            while ((done and not memory.empty()) or (memory.size() >= N_STEP_RETURN)) {
                const auto experience = pick_experience(memory);
                experiences_queue.push(experience);
                memory.pop_front();
            }

            curr_state = next_state;
        }
    }
}

/******************************************************************************/

void fit(const std::vector<experience_t>& batch)
{
    std::vector<gym_uds::observation_t> states;
    std::vector<gym_uds::action_t> actions;
    std::vector<float> rewards;

    for (const auto& experience: batch) {
        const auto curr_state = std::get<0>(experience);
        const auto action = std::get<1>(experience);
        const auto reward = std::get<2>(experience);
        states.push_back(curr_state);
        actions.push_back(action);
        rewards.push_back(reward);
    }
    model.fit(states, actions, rewards);
}

void trainer()
{
    while (num_training_experiences < MAX_NUM_TRAINING_EXPERIENCES) {
        const auto batch_size = std::min(BATCH_SIZE, MAX_NUM_TRAINING_EXPERIENCES-num_training_experiences);

        auto batch = std::vector<experience_t>(batch_size);
        for (auto i = 0; i < batch.size(); ++i) {
            batch[i] = experiences_queue.pop();
        }
        fit(batch);
        num_training_experiences += batch_size;
    }
}

/******************************************************************************/

int main(int argc, char const *argv[])
{
    assert (N_STEP_RETURN >= 1);

    // start the trainer
    auto trainer_thread = std::thread(trainer);

    // start the agents
    auto agents_threads = std::vector<std::thread>(NUM_AGENTS);
    for (auto i = 0; i < NUM_AGENTS; ++i) {
        agents_threads[i] = std::thread(agent, i);
    }

    // and wait for them to finish
    for (auto i = 0; i < NUM_AGENTS; ++i) {
        agents_threads[i].join();
    }
    trainer_thread.join();
    assert(num_training_experiences == MAX_NUM_TRAINING_EXPERIENCES);

    // save the trained model
    model.save();
}
