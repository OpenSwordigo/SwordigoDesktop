/* Preserve SceneLoadingView::AnimateIn's complete incoming ABI, including the
 * x8 indirect-result register emitted by the original ARM64 function. */

    .text
    .align 2
    .global sre_SceneLoadingView_AnimateIn
    .type sre_SceneLoadingView_AnimateIn, %function
sre_SceneLoadingView_AnimateIn:
    adrp x16, :got:g_orig_SceneLoadingView_AnimateIn
    ldr  x16, [x16, :got_lo12:g_orig_SceneLoadingView_AnimateIn]
    ldr  x16, [x16]
    cbz  x16, 1f
    br   x16
1:
    ret
    .size sre_SceneLoadingView_AnimateIn, .-sre_SceneLoadingView_AnimateIn
