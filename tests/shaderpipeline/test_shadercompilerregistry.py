from panda3d import core


def test_shadercompilerregistry_exists(registry):
    assert registry is not None


#def test_shadercompilerregistry_glslpreproc_loaded(registry):
#    assert core.ShaderCompilerGlslPreProc in [type(i) for i in registry.compilers]
#    assert registry.get_compiler_for_language(core.Shader.SL_GLSL) is not None

#def test_shadercompilerregistry_missing_lang(registry):
#    assert core.ShaderCompilerGlslPreProc in [type(i) for i in registry.compilers]
#    assert registry.get_compiler_for_language(core.Shader.SL_none) is None
