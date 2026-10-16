// Published releases keep their tested assets, even if publishing creates a tag push.
module.exports = async function releaseState({ github, context, core, tag }) {
  let release;
  let missing = false;
  try {
    ({ data: release } = await github.rest.repos.getReleaseByTag({ ...context.repo, tag }));
  } catch (error) {
    // This endpoint can hide drafts. Both missing releases and drafts may be built.
    if (error.status !== 404) throw error;
    missing = true;
  }
  if (!missing &&
      (!release || release.tag_name !== tag || typeof release.draft !== 'boolean')) {
    throw new Error('Unexpected release response; refusing to change release assets');
  }
  const build = missing || release.draft;
  core.setOutput('build', String(build));
  core.info(build
    ? `No published release for ${tag}; a draft may be built.`
    : `Release ${tag} is already published; keeping its verified assets.`);
  return build;
};
