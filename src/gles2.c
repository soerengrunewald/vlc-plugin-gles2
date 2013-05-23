/*****************************************************************************
 * gles2.c: OpenGL ES2 video output display (with linear deinterlacing)
 *****************************************************************************
 * Copyright (C) 2000-2013 VLC authors and VideoLAN
 *
 * Authors: Soeren Grunewald <soeren.grunewald@avionic-design.de>
 *          Julian Scheel <julian@jusst.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xatom.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_picture_pool.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>

#include <vlc/libvlc.h>

#ifndef N_
#define N_(x) x
#endif

/* FIXME: This should come form GLES2/gl2.h */
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

#define GLES2_TEXT N_("OpenGL ES 2 extension")
#define PROVIDER_LONGTEXT N_("Extension through which to use the OpenGL ES2.")

#define CHROMA_TEXT N_("Chroma used")
#define CHROMA_LONGTEXT N_("Force use of a specific chroma for output. Default is I420.")

#define DISPLAY_TEXT N_("X11 display")
#define DISPLAY_LONGTEXT N_( \
	"Video will be rendered with this X11 display. " \
	"If empty, the default display will be used.")

#define XID_TEXT N_("X11 window ID")
#define XID_LONGTEXT N_( \
	"Video will be embedded in this pre-existing window. " \
	"If zero, a new window will be created.")


static int Open( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VOUT )
    set_shortname( N_("gles2") )
    set_description( N_("OpenGL ES 2 video output") )
    set_capability( "vout display", 151 )
    set_callbacks( Open, Close )

    add_shortcut("embed-xid", "xid")
    add_module("gles2", "opengl es2", NULL, GLES2_TEXT, PROVIDER_LONGTEXT, true)
    add_string("chroma", NULL, CHROMA_TEXT, CHROMA_LONGTEXT, true)
    add_string("x11-display", NULL, DISPLAY_TEXT, DISPLAY_LONGTEXT, true)
    add_integer("drawable-xid", 0, XID_TEXT, XID_LONGTEXT, true)
        change_volatile ()

vlc_module_end ()


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
enum shader_types {
	SHADER_TYPE_DEINT_LINEAR,
	SHADER_TYPE_COPY
};

typedef struct rectangle_t {
	unsigned x;
	unsigned y;
	unsigned width;
	unsigned height;
} rectangle_t;

typedef struct {
	GLuint id;
	GLint  loc;
} gl_texture_t;

typedef struct {
	GLint  program;
	GLuint vertex;
	GLuint fragment;
	GLint  position_loc;
	GLint  texcoord_loc;
} gl_shader_t;

typedef struct opengl_es2_t {
	GLuint       framebuffer;
	gl_shader_t  deint;
	gl_shader_t  scale;
	gl_texture_t tex[3];  /* y,u,v textures */
	gl_texture_t rgb_tex; /* the rgb output */

	rectangle_t viewport;
	/* do we have support for GL_UNPACK_ROW_LENGTH */
	bool has_unpack_row;
} opengl_es2_t;

typedef struct egl_backend_t {
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
} egl_backend_t;

typedef struct x11_backend_t {
	Display     *display;
	Window      window;
	rectangle_t rect;
	bool        external;
} x11_backend_t;

typedef struct vout_display_sys_t {
	vout_display_t *vd;
	x11_backend_t  *x11;
	egl_backend_t  *egl;
	opengl_es2_t   *gl;
	picture_pool_t *pool;
} vout_display_sys_t;


static void compute_bounding_box(const vout_display_cfg_t *cfg,
				       const rectangle_t *dst,
				       rectangle_t *res)
{
	double src_ratio;
	double dst_ratio;
	rectangle_t src;

	src.x = src.y = 0;
	src.width = cfg->display.width;
	src.height = cfg->display.height;

	src_ratio = (double)src.width / src.height;
	dst_ratio = (double)dst->width / dst->height;

	if (src_ratio > dst_ratio) {
		res->width  = dst->width;
		res->height = dst->width / src_ratio;
		res->x      = 0;
		res->y      = (dst->height - res->height) / 2;
	} else if (src_ratio < dst_ratio) {
		res->width  = dst->height * src_ratio;
		res->height = dst->height;
		res->x      = (dst->width - res->width) / 2;
		res->y      = 0;
	} else {
		res->width  = dst->width;
		res->height = dst->height;
		res->x = res->y = 0;
	}
}

static void x11_backend_destroy(x11_backend_t *x11)
{
	if (!x11)
		return;

	if (x11->window) {
		XLockDisplay(x11->display);
		if (!x11->external)
			XDestroyWindow(x11->display, x11->window);
		else
			XSelectInput(x11->display, x11->window, 0);
		x11->window = 0;
		XUnlockDisplay(x11->display);
	}

	if (x11->display) {
		XCloseDisplay(x11->display);
		x11->display = NULL;
	}

	free(x11);
	x11 = NULL;
}

static void x11_backend_handle_events(vout_display_sys_t *sys)
{
	x11_backend_t *x11 = sys->x11;
	XEvent xev;

	while (XPending(x11->display)) {
		XNextEvent(x11->display, &xev);
		if (xev.type == ConfigureNotify) {
			x11->rect.width = xev.xconfigure.width;
			x11->rect.height = xev.xconfigure.height;

			compute_bounding_box(sys->vd->cfg, &x11->rect,
					     &sys->gl->viewport);
		}
	}
}

static int x11_backend_create(x11_backend_t **x11, vout_window_cfg_t *cfg, vout_display_t *vd)
{
	x11_backend_t *x;
	Window external;
	Window root;

	x = calloc(1, sizeof(*x));
	if (!x)
		return VLC_ENOMEM;

	x->rect.x = cfg->x;
	x->rect.y = cfg->y;
	x->rect.width  = cfg->width;
	x->rect.height = cfg->height;

	if (unlikely(x->rect.width == -1 && x->rect.height == -1)) {
		x->rect.width  = 768;
		x->rect.height = 576;
	}

	external = var_InheritInteger(vd, "drawable-xid");
	if (external != 0) {
		x->external = true;
		x->window = external;
	}

	x->display = XOpenDisplay(NULL);
	if (!x->display) {
		fprintf(stderr, "ERR: %s: Could not create X display\n", __func__);
		goto cleanup;
	}

	XLockDisplay(x->display);

	root = DefaultRootWindow(x->display);

	if (x->external) {
		unsigned int border, depth;

		XSelectInput(x->display, x->window, ExposureMask |
			     StructureNotifyMask | VisibilityChangeMask);
		XGetGeometry(x->display, x->window, &root,
			     (int*)&x->rect.x, (int*)&x->rect.y,
			     (uint*)&x->rect.width, (uint*)&x->rect.height,
			     &border, &depth);
		XFlush(x->display);
	} else {
		XSetWindowAttributes swa;
		XWMHints hints;

		swa.event_mask = (StructureNotifyMask | ExposureMask
				  | VisibilityChangeMask);

		x->window = XCreateWindow(x->display, root, x->rect.x, x->rect.y,
					  x->rect.width, x->rect.height, 0,
					  CopyFromParent, InputOutput,
					  CopyFromParent, CWEventMask,
					  &swa);

		XSetWindowBackgroundPixmap(x->display, x->window, None);

		hints.input = True;
		hints.flags = InputHint;

		XSetWMHints(x->display, x->window, &hints);
		XMapWindow(x->display, x->window);
		XFlush(x->display);

		XStoreName(x->display, x->window, "VLC OpenGL ES2");
	}

	XUnlockDisplay(x->display);

//	fprintf(stderr, "MSG: using %s windows at: x=%d, y=%d, w=%d, h=%d\n",
//			x->external ? "external" : "internal",
//			x->rect.x, x->rect.y, x->rect.width, x->rect.height);

	*x11 = x;
	return VLC_SUCCESS;

cleanup:
	free(x);
	return VLC_EGENERIC;
}

/*
 * Tegra specific fix to close handles left open by the tegra driver.
 * This has only relevance if you keep the player process open and
 * start stop the video playback very offen or over a very long time.
 */
static void quirk_close_forgotten_handles()
{
	char path[MAXPATHLEN];
	struct dirent *entry;
	struct stat info;
	int64_t fid;
	DIR *dir;

	snprintf(path, ARRAY_SIZE(path), "/proc/%u/fd", getpid());

	if (lstat(path, &info) < 0 || !S_ISDIR(info.st_mode))
		return;

	if ((dir = opendir(path)) == NULL)
		return;

	while ((entry = readdir(dir)) != NULL) {
		char file[MAXPATHLEN] = { 0 };
		char link[MAXPATHLEN] = { 0 };

		snprintf(file, ARRAY_SIZE(file), "%s/%s", path, entry->d_name);

		if (lstat(file, &info) < 0 || !S_ISLNK(info.st_mode))
			continue;

		if (readlink(file, link, ARRAY_SIZE(link)) < 0)
			continue;

		/* if the link does not point to on of these files,
		 * try the next one */
		if (strcmp(link, "/dev/tegra_sema") != 0 &&
		    strcmp(link, "/dev/nvhost-gr2d") != 0 &&
		    strcmp(link, "/dev/nvhost-gr3d") != 0)
			continue;

		fid = strtoll(entry->d_name, NULL, 10);
		if (fid > 0 && close(fid) < 0)
			fprintf(stderr, "ERR: failed to close handle %lld\n", fid);
	}
	closedir(dir);
}

static void egl_backend_destroy(egl_backend_t *egl)
{
	if (!egl)
		return;

	if (egl->context) {
		eglDestroyContext(egl->display, egl->context);
		egl->context = NULL;
	}
	if (egl->surface) {
		eglDestroySurface(egl->display, egl->surface);
		egl->surface = NULL;
	}
	if (egl->display) {
		eglTerminate(egl->display);
		egl->display = NULL;
	}
	free(egl);
	egl = NULL;

	/* FIXME: this is required due to a tegra l4t driver bug. */
	quirk_close_forgotten_handles();
}

static int egl_backend_create(egl_backend_t **egl, x11_backend_t *x11)
{
	const EGLint cfg_attr[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_BUFFER_SIZE, 24,
		EGL_NONE
	};
	const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLint major= 0, minor = 0;
	egl_backend_t *e;
	EGLBoolean ret;
	EGLConfig cfg;
	EGLint num;

	e = malloc(sizeof(*e));
	if (unlikely(e == NULL)) {
		fprintf(stderr, "ERR: %s: malloc failed\n", __func__);
		return VLC_ENOMEM;
	}

	e->display = eglGetDisplay(x11->display);
	if (e->display == EGL_NO_DISPLAY) {
		fprintf(stderr, "ERR: %s: eglGetDisplay failed: 0x%x\n",
			__func__, eglGetError());
		goto cleanup;
	}

	ret = eglInitialize(e->display, &major, &minor);
	if (!ret || major != 1 || minor < 2) {
		fprintf(stderr, "ERR: %s: eglInitialize failed: %d.%d: 0x%x\n",
			__func__, major, minor, eglGetError());
		goto cleanup;
	}

//	fprintf(stderr, "EGL version %s by %s\n",
//		eglQueryString(e->display, EGL_VERSION),
//		eglQueryString(e->display, EGL_VENDOR));

	ret = eglBindAPI(EGL_OPENGL_ES_API);
	if (!ret) {
		fprintf(stderr, "ERR: %s: eglBindAPI failed: 0x%x\n",
			__func__, eglGetError());
		goto cleanup;
	}

	ret = eglChooseConfig(e->display, cfg_attr, &cfg, 1, &num);
	if (!ret) {
		fprintf(stderr, "ERR: %s: eglChooseConfig failed: 0x%x\n",
			__func__, eglGetError());
		goto cleanup;
	}

	e->surface = eglCreateWindowSurface(e->display, cfg, x11->window, NULL);
	if (e->surface == EGL_NO_SURFACE) {
		fprintf(stderr, "ERR: %s: eglCreateWindowSurface failed: 0x%x\n",
			__func__, eglGetError());
		goto cleanup;
	}

	e->context = eglCreateContext(e->display, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (e->context == EGL_NO_CONTEXT) {
		fprintf(stderr, "ERR: %s: eglCreateContext failed: 0x%x\n",
			__func__, eglGetError());
		goto cleanup;
	}

	ret= eglMakeCurrent(e->display, e->surface, e->surface, e->context);
	if (!ret) {
		fprintf(stderr, "ERR: %s: eglMakeCurrent failed: 0x%x\n",
			__func__, eglGetError());
		goto cleanup;
	}

	*egl = e;
	return VLC_SUCCESS;

cleanup:
	egl_backend_destroy(e);
	return VLC_EGENERIC;
}

static void shader_delete(gl_shader_t *shader)
{
	if (shader->vertex) {
		glDeleteShader(shader->vertex);
		shader->vertex = 0;
	}
	if (shader->fragment) {
		glDeleteShader (shader->fragment);
		shader->fragment = 0;
	}
	if (shader->program) {
		glDeleteProgram (shader->program);
		shader->program = 0;
	}
}

static int shader_load_source(const GLchar *src, GLenum type)
{
	GLint compiled;
	GLuint s;

	s = glCreateShader(type);
	if (!s) {
		fprintf(stderr, "ERR: %s: glCreateShader failed\n",
			__func__);
		return 0;
	}

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);

	glGetShaderiv(s, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		GLint len = 0;
		glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
		if (len > 0) {
			char *info = alloca(sizeof(char) * len);
			glGetShaderInfoLog(s, len, NULL, info);

			fprintf(stderr, "ERR: %s\n", info);
		}
		glDeleteShader(s);
		return 0;
	}

	return s;
}

static int shader_load(gl_shader_t *shader, enum shader_types type)
{
	static const GLchar vertex[] = {
		"attribute vec4 vPosition;\n"
		"attribute vec2 aTexcoord;\n"
		"varying vec2 vTexcoord;\n"
		"\n"
		"void main() {\n"
		"	gl_Position = vPosition;\n"
		"	vTexcoord = aTexcoord;\n"
		"}"
	};
	static const GLchar fragment_copy[] = {
		"precision mediump float;\n"
		"varying vec2 vTexcoord;\n"
		"uniform sampler2D s_tex;\n"
		"uniform float line_height;\n"
		"\n"
		"void main() {\n"
		"	gl_FragColor = vec4(texture2D(s_tex, vTexcoord).rgb, 1.0);\n"
		"}"
	};
	static const GLchar fragment_deint[] = {
		"precision mediump float;\n"
		"\n"
		"varying vec2 vTexcoord;\n"
		"\n"
		"uniform sampler2D s_ytex;\n"
		"uniform sampler2D s_utex;\n"
		"uniform sampler2D s_vtex;\n"
		"uniform float line_height;\n"
		"\n"
		"void main() {\n"
		"	float y1, y2, u1, u2, v1, v2;\n"
		"	float r, g, b;\n"
		"	float y, u, v;\n"
		"	vec2 tmpcoord;\n"
		"	vec2 tmpcoord_2;\n"
		"\n"
		"	tmpcoord.x = vTexcoord.x;\n"
		"	tmpcoord.y = vTexcoord.y + line_height;\n"
		"	tmpcoord_2.x = vTexcoord.x;\n"
		"	tmpcoord_2.y = vTexcoord.y + line_height*2.0;\n"
		"\n"
		"	y1 = texture2D(s_ytex, vTexcoord).r;\n"
		"	y2 = texture2D(s_ytex, tmpcoord).r;\n"
		"	u1 = texture2D(s_utex, vTexcoord).r;\n"
		"	u2 = texture2D(s_utex, tmpcoord_2).r;\n"
		"	v1 = texture2D(s_vtex, vTexcoord).r;\n"
		"	v2 = texture2D(s_vtex, tmpcoord_2).r;\n"
		"\n"
		"	y = mix (y1, y2, 0.5);\n"
		"	u = mix (u1, u2, 0.5);\n"
		"	v = mix (v1, v2, 0.5);\n"
		"\n"
		"	y = 1.1643 * (y - 0.0625);\n"
		"	u = u - 0.5;\n"
		"	v = v - 0.5;\n"
		"\n"
		"	r = y + 1.5958 * v;\n"
		"	g = y - 0.39173 * u - 0.81290 * v;\n"
		"	b = y + 2.017 * u;\n"
		"\n"
		"	gl_FragColor = vec4(r, g, b, 1.0);\n"
		"}"
	};
	const GLchar *fragment;

	if (type == SHADER_TYPE_DEINT_LINEAR)
		fragment = fragment_deint;
	else
		fragment = fragment_copy;

	shader->vertex = shader_load_source(vertex, GL_VERTEX_SHADER);
	if (shader->vertex == 0) {
		fprintf(stderr, "ERR: %s: shader_load_source(vertex) failed\n",
			__func__);
		return -1;
	}

	shader->fragment = shader_load_source(fragment, GL_FRAGMENT_SHADER);
	if (shader->fragment == 0) {
		fprintf(stderr, "ERR: %s: shader_load_source(fragment) failed\n",
			__func__);
		return -1;
	}
	return 0;
}

static int shader_init(gl_shader_t *shader, enum shader_types type)
{
	int linked, ret;
	GLint err;

	shader->program = glCreateProgram();
	if (shader->program == 0) {
		fprintf(stderr, "ERR: %s: glCreateProgram failed %d\n",
			__func__, glGetError());
		return -1;
	}

	ret = shader_load(shader, type);
	if (ret < 0) {
		fprintf(stderr, "ERR: %s: shader_load failed\n", __func__);
		goto failure;
	}

	glAttachShader(shader->program, shader->vertex);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "ERR: %s: glAttachShader(vertex) failed %d\n",
			__func__, err);
		goto failure;
	}

	glAttachShader(shader->program, shader->fragment);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "ERR: %s: glAttachShader(fragment) failed %d\n",
			__func__, err);
		goto failure;
	}

	glBindAttribLocation(shader->program, 0, "vPosition");
	glLinkProgram(shader->program);

	glGetProgramiv(shader->program, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLint len = 0;
		glGetProgramiv(shader->program, GL_INFO_LOG_LENGTH, &len);
		if (len > 0) {
			char *info = alloca(sizeof(char) * len);
			glGetProgramInfoLog(shader->program, len, NULL, info);

			fprintf(stderr, "ERR: %s: %s\n", __func__, info);
		}
		glDeleteProgram(shader->program);
	}

	glUseProgram(shader->program);

	shader->position_loc = glGetAttribLocation(shader->program, "vPosition");
	shader->texcoord_loc = glGetAttribLocation(shader->program, "aTexcoord");

	glClearColor(0.0, 0.0, 0.0, 1.0);
	return 0;

failure:
	fprintf(stderr, "ERR: %s: oh no!!! %d\n", __func__, glGetError());
	shader_delete(shader);
	return -1;
}

static GLuint texture_create(GLenum type)
{
	GLuint tex = 0;

	glGenTextures(1, &tex);

	glBindTexture(GL_TEXTURE_2D, tex);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, type);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, type);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	return tex;
}

/*
 * TODO: This function shall be used if we do not have GL_UNPACK_ROW_LENGTH
 * support. Therefor we need to strip the data we get before we load it into
 * the textures.
 */
static void update_textures_complex(vout_display_sys_t *vout, picture_t *p)
{
	static const vlc_chroma_description_t *c = NULL;
	const video_format_t *const f = &vout->vd->fmt;
	opengl_es2_t *gl = vout->gl;
	GLbyte *buf, *dst, *src;

	fprintf(stderr, "> %s()\n", __func__);

	if (!c)
		c = vlc_fourcc_GetChromaDescription(f->i_chroma);

	for (unsigned i = 0; i < p->i_planes; i++) {
		unsigned rows = f->i_visible_height * c->p[i].h.num / c->p[i].h.den;
		unsigned line = f->i_visible_width * c->p[i].w.num / c->p[i].w.den;

		dst = buf = alloca(line * rows);
		src = p->p[i].p_pixels;

		for (unsigned r = 0; r < rows; r++) {
			memcpy(dst, src, line);
			src += p->p[i].i_pitch / p->p[i].i_pixel_pitch;
			dst += line;
		}

		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, gl->tex[i].id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, line, rows,
			     0, GL_LUMINANCE, GL_UNSIGNED_BYTE, buf);
		glUniform1i(gl->tex[i].loc, i);
	}
	fprintf(stderr, "< %s()\n", __func__);
}

static void update_textures_simple(vout_display_sys_t *vout, picture_t *p)
{
	opengl_es2_t *gl = vout->gl;

	for (unsigned i = 0; i < p->i_planes; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, gl->tex[i].id);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, p->p[i].i_pitch / p->p[i].i_pixel_pitch);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, p->p[i].i_visible_pitch, p->p[i].i_visible_lines,
		             0, GL_LUMINANCE, GL_UNSIGNED_BYTE, p->p[i].p_pixels);
		glUniform1i(gl->tex[i].loc, i);
	}
	/* reset row packing */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void update_textures(vout_display_sys_t *vout, picture_t *p)
{
	if (vout->gl->has_unpack_row)
		update_textures_simple(vout, p);
	else
		update_textures_complex(vout, p);
}

static void do_deinterlace_and_color_conversion(vout_display_sys_t *vout, picture_t *p)
{
	const GLfloat vVertices[] = {
		-1.0f, -1.0f, 0.0f, 1.0f,
		 1.0f, -1.0f, 1.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f, 0.0f,
	};
	const GLushort indices[] = {
		0, 1, 2, 0, 2, 3
	};
	const GLuint height = p->format.i_height;
	const GLuint width = p->format.i_width;
	opengl_es2_t *gl = vout->gl;
	GLint line_height_loc;

	glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
	glUseProgram(gl->deint.program);

	glViewport(0, 0, width, height);

	glVertexAttribPointer(gl->deint.position_loc, 2,
			      GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      vVertices);
	glVertexAttribPointer(gl->deint.texcoord_loc, 2,
			      GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      &vVertices[2]);

	glEnableVertexAttribArray(gl->deint.position_loc);
	glEnableVertexAttribArray(gl->deint.texcoord_loc);

	update_textures(vout, p);

	line_height_loc = glGetUniformLocation(gl->deint.program, "line_height");
	glUniform1f(line_height_loc, 1.0/height);

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
}

static void do_scaling(vout_display_sys_t *vout,
					    picture_t *p)
{
	const GLfloat vVertices[] = {
		-1.0f, -1.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 0.0f,
		 1.0f,  1.0f, 1.0f, 1.0f,
		-1.0f,  1.0f, 0.0f, 1.0f,
	};
	const GLushort indices[] = {
		0, 1, 2, 0, 2, 3
	};
	opengl_es2_t *gl = vout->gl;

	glUseProgram(gl->scale.program);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glViewport(gl->viewport.x, gl->viewport.y,
			gl->viewport.width, gl->viewport.height);

	glClear(GL_COLOR_BUFFER_BIT);

	glVertexAttribPointer(gl->scale.position_loc, 2,
			      GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      vVertices);
	glVertexAttribPointer(gl->scale.texcoord_loc, 2,
			      GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      &vVertices[2]);

	glEnableVertexAttribArray(gl->scale.position_loc);
	glEnableVertexAttribArray(gl->scale.texcoord_loc);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, gl->rgb_tex.id);
	glUniform1i(gl->rgb_tex.loc, 3);

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
}

static void opengl_es2_destroy(opengl_es2_t *gl)
{
	if (!gl)
		return;

	const GLuint framebuffers[] = {
		gl->framebuffer
	};
	const GLuint textures[] = {
		gl->tex[Y_PLANE].id,
		gl->tex[U_PLANE].id,
		gl->tex[V_PLANE].id,
		gl->rgb_tex.id
	};

	shader_delete(&gl->deint);
	shader_delete(&gl->scale);

	glDeleteTextures(ARRAY_SIZE(textures), textures);
	glDeleteFramebuffers(ARRAY_SIZE(framebuffers), framebuffers);

	memset(gl, 0, sizeof(*gl));
	free(gl);
	gl = NULL;
}

static inline bool opengl_have_extention(const char *extentions, const char *search)
{
	size_t len = strlen(search);
	while (extentions) {
		while (*extentions == ' ')
			extentions++;
		if ((strncmp(extentions, search, len) == 0) &&
		    memchr(" ", extentions[len], 2))
			return true;
		extentions = strchr(extentions, ' ');
	}
	return false;
}

static int opengl_es2_create(opengl_es2_t **p_gl)
{
	opengl_es2_t *gl;

	gl = calloc(1, sizeof(*gl));
	if (!gl)
		return VLC_ENOMEM;

	if (shader_init(&gl->deint, SHADER_TYPE_DEINT_LINEAR) < 0) {
		fprintf(stderr, "ERR: %s: shader_init(DEINT)\n", __func__);
		goto cleanup;
	}

	gl->tex[Y_PLANE].id = texture_create(GL_NEAREST);
	gl->tex[Y_PLANE].loc = glGetUniformLocation(gl->deint.program, "s_ytex");

	gl->tex[U_PLANE].id = texture_create(GL_NEAREST);
	gl->tex[U_PLANE].loc = glGetUniformLocation(gl->deint.program, "s_utex");

	gl->tex[V_PLANE].id = texture_create(GL_NEAREST);
	gl->tex[V_PLANE].loc = glGetUniformLocation(gl->deint.program, "s_vtex");

	if (shader_init(&gl->scale, SHADER_TYPE_COPY) < 0) {
		fprintf(stderr, "ERR: %s: shader_init(SCALE)\n", __func__);
		goto cleanup;
	}
	gl->rgb_tex.loc = glGetUniformLocation(gl->scale.program, "s_tex");

	/* The rest is done when pool is requested */

#if GL_UNPACK_ROW_LENGTH
	/* check for extentions we can use */
	{
		const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
		fprintf(stderr, "MSG: available extentions:\n   %s\n", extensions);

		gl->has_unpack_row = opengl_have_extention(extensions, "GL_EXT_unpack_subimage");
		fprintf(stderr, "MSG: have %sunpack_row support\n",
			gl->has_unpack_row ? "" : "no ");
	}
#endif

	*p_gl = gl;
	return VLC_SUCCESS;

cleanup:
	shader_delete(&gl->deint);
	shader_delete(&gl->scale);

	free(gl);
	gl = NULL;
	return VLC_EGENERIC;
}

static picture_pool_t *do_pool(vout_display_t *, unsigned count);
static void           do_display(vout_display_t *, picture_t *, subpicture_t *);
static int            do_control(vout_display_t *, int, va_list);

static int Open(vlc_object_t *object)
{
	vout_display_t *vd = (vout_display_t *)object;
	vout_display_sys_t *sys;
	vout_window_cfg_t *cfg;

	vd->sys = sys = calloc(1, sizeof(*sys));
	if (!sys)
		return VLC_ENOMEM;

	sys->vd   = vd;
	sys->pool = NULL;

	cfg = alloca(sizeof(*cfg));
	cfg->x = var_InheritInteger(vd, "video-x");
	cfg->y = var_InheritInteger(vd, "video-y");

	if (vd->cfg) {
		cfg->width  = vd->cfg->display.width;
		cfg->height = vd->cfg->display.height;
	} else {
		cfg->width  = -1;
		cfg->height = -1;
	}
	cfg->type = VOUT_WINDOW_TYPE_XID;

	if (x11_backend_create(&sys->x11, cfg, vd) != VLC_SUCCESS) {
		fprintf(stderr, "ERR: %s: failed to create x11\n", __func__);
		goto cleanup;
	}
	if (egl_backend_create(&sys->egl, sys->x11) != VLC_SUCCESS) {
		fprintf(stderr, "ERR: %s: failed to create egl\n", __func__);
		goto cleanup;
	}
	if (opengl_es2_create(&sys->gl) != VLC_SUCCESS) {
		fprintf(stderr, "ERR: %s: failed to create gles2\n", __func__);
		goto cleanup;
	}

	compute_bounding_box(vd->cfg, &sys->x11->rect, &sys->gl->viewport);

	/* p_vd->info is not modified */
	vd->fmt.i_chroma = VLC_CODEC_I420;

	vd->pool    = do_pool;
	vd->prepare = NULL;
	vd->display = do_display;
	vd->control = do_control;
	vd->manage  = NULL;

	return VLC_SUCCESS;

cleanup:
	opengl_es2_destroy(sys->gl);
	egl_backend_destroy(sys->egl);
	x11_backend_destroy(sys->x11);
	free(sys);
	return VLC_EGENERIC;
}

static void Close(vlc_object_t *object)
{
	vout_display_t *vd = (vout_display_t *)object;
	vout_display_sys_t *sys = vd->sys;

	opengl_es2_destroy(sys->gl);
	egl_backend_destroy(sys->egl);
	x11_backend_destroy(sys->x11);

	if (sys->pool)
		picture_pool_Delete(sys->pool);

	free(sys);
	sys = NULL;
}

/*
 * Return a pointer over the current picture_pool_t* (mandatory).
 *
 * For performance reasons, it is best to provide at least count
 * pictures but it is not mandatory.
 *
 * You can return NULL when you cannot/do not want to allocate
 * pictures.
 *
 * The vout display module keeps the ownership of the pool and can
 * destroy it only when closing or on invalid pictures control.
 */
static picture_pool_t *do_pool(vout_display_t *vd, unsigned count)
{
	vout_display_sys_t *sys = vd->sys;

	if (!sys->pool) {
		opengl_es2_t *gl = sys->gl;

		sys->pool = picture_pool_NewFromFormat(&vd->fmt, count);

		/* create the framebuffer and the corresponding texture */
		glGenFramebuffers(1, &gl->framebuffer);

		gl->rgb_tex.id = texture_create(GL_LINEAR);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
			     vd->fmt.i_width, vd->fmt.i_height,
			     0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

		glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				       GL_TEXTURE_2D, gl->rgb_tex.id, 0);
	}
	return sys->pool;
}

/*
 * Display a picture and an optional subpicture (mandatory).
 *
 * The picture and the optional subpicture must be displayed as soon as
 * possible.
 * You cannot change the pixel content of the picture_t or of the
 * subpicture_t.
 *
 * This function gives away the ownership of the picture and of the
 * subpicture, so you must release them as soon as possible.
 */
static void do_display(vout_display_t *vd, picture_t *p, subpicture_t *sp)
{
	vout_display_sys_t *sys = vd->sys;
	egl_backend_t *egl = sys->egl;

	if (p->format.i_chroma != VLC_CODEC_I420 || p->i_planes != 3) {
		fprintf(stderr, "ERR: unsupported picture format: %s\n",
			vlc_fourcc_GetDescription(UNKNOWN_ES, p->format.i_chroma));
		return;
	}

	/* do event handling stuff */
	x11_backend_handle_events(sys);
	/* do the rendering */
	do_deinterlace_and_color_conversion(sys, p);
	do_scaling(sys, p);
	/* do the acutall drawing */
	eglSwapBuffers(egl->display, egl->surface);

	picture_Release(p);
	if (sp)
		subpicture_Delete(sp);
}

static int do_control(vout_display_t *vd, int query, va_list args)
{
	vout_display_sys_t *vout = vd->sys;

	switch (query) {
	case VOUT_DISPLAY_HIDE_MOUSE:
		fprintf(stderr, "MSG: VOUT_DISPLAY_HIDE_MOUSE\n");
		return VLC_SUCCESS;

	case VOUT_DISPLAY_CHANGE_FULLSCREEN:
		fprintf(stderr, "MSG: VOUT_DISPLAY_CHANGE_FULLSCREEN\n");
		return VLC_SUCCESS;

	case VOUT_DISPLAY_CHANGE_WINDOW_STATE: {
		const unsigned state = va_arg(args, unsigned);
		fprintf(stderr, "MSG: VOUT_DISPLAY_CHANGE_WINDOW_STATE -> state=%d\n", state);
		} return VLC_SUCCESS;

	case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
	case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT: {
		const vout_display_cfg_t *cfg = va_arg(args, const vout_display_cfg_t *);
		fprintf(stderr, "MSG: VOUT_DISPLAY_CHANGE_DISPLAY_SIZE\n");
		compute_bounding_box(cfg, &vout->x11->rect, &vout->gl->viewport);
		} return VLC_SUCCESS;

	default:
		msg_Err(vd, "Unsupported query %d in vout display gles2", query);
		return VLC_EGENERIC;
	}
}
