const assert = require('node:assert/strict');
const test = require('node:test');
const releaseState = require('./release-state.cjs');

async function check(response, error) {
  const outputs = {};
  const build = await releaseState({
    github: { rest: { repos: { getReleaseByTag: async (request) => {
      assert.deepEqual(request, { owner: 'owner', repo: 'repo', tag: 'v0.2.0' });
      if (error) throw error;
      return { data: response };
    } } } },
    context: { repo: { owner: 'owner', repo: 'repo' } },
    core: { setOutput: (name, value) => { outputs[name] = value; }, info: () => {} },
    tag: 'v0.2.0',
  });
  assert.equal(outputs.build, String(build));
  return build;
}

test('published releases and prereleases cannot be rebuilt', async () => {
  for (const prerelease of [false, true]) {
    assert.equal(await check({ tag_name: 'v0.2.0', draft: false, prerelease }), false);
  }
});

test('drafts and missing releases may be built', async () => {
  assert.equal(await check({ tag_name: 'v0.2.0', draft: true }), true);
  assert.equal(await check(null, { status: 404 }), true);
});

test('API and network failures stop the workflow instead of allowing writes', async () => {
  for (const status of [401, 403, 429, 500, undefined]) {
    const error = Object.assign(new Error('request failed'), { status });
    await assert.rejects(check(null, error), (actual) => actual === error);
  }
});

test('unexpected release metadata cannot authorize a build', async () => {
  for (const response of [undefined, null, {}, { tag_name: 'different', draft: true },
                          { tag_name: 'v0.2.0', draft: 'false' }]) {
    await assert.rejects(check(response), /Unexpected release response/);
  }
});
