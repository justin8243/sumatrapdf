// Copyright (C) 2004-2021 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 1305 Grant Avenue - Suite 200, Novato,
// CA 94945, U.S.A., +1(415)492-9861, for further information.

#include "mupdf/fitz.h"
#include "pdf-annot-imp.h"

#include <string.h>

static pdf_obj *
resolve_dest_rec(fz_context *ctx, pdf_document *doc, pdf_obj *dest, int depth)
{
	if (depth > 10) /* Arbitrary to avoid infinite recursion */
		return NULL;

	if (pdf_is_name(ctx, dest) || pdf_is_string(ctx, dest))
	{
		dest = pdf_lookup_dest(ctx, doc, dest);
		dest = resolve_dest_rec(ctx, doc, dest, depth+1);
		return dest;
	}

	else if (pdf_is_array(ctx, dest))
	{
		return dest;
	}

	else if (pdf_is_dict(ctx, dest))
	{
		dest = pdf_dict_get(ctx, dest, PDF_NAME(D));
		return resolve_dest_rec(ctx, doc, dest, depth+1);
	}

	else if (pdf_is_indirect(ctx, dest))
		return dest;

	return NULL;
}

static pdf_obj *
resolve_dest(fz_context *ctx, pdf_document *doc, pdf_obj *dest)
{
	return resolve_dest_rec(ctx, doc, dest, 0);
}

char *
pdf_parse_link_dest(fz_context *ctx, pdf_document *doc, pdf_obj *dest)
{
	float arg1, arg2, arg3, arg4;
	fz_link_dest destination;
	pdf_obj *pageobj, *typeobj;
	fz_matrix page_ctm;
	const char *ld;
	int pageno;
	fz_point p;
	fz_rect rect;

	dest = resolve_dest(ctx, doc, dest);
	if (dest == NULL)
	{
		fz_warn(ctx, "undefined link destination");
		return NULL;
	}

	if (pdf_is_name(ctx, dest))
	{
		ld = pdf_to_name(ctx, dest);
		return fz_strdup(ctx, ld);
	}
	else if (pdf_is_string(ctx, dest))
	{
		ld = pdf_to_str_buf(ctx, dest);
		return fz_strdup(ctx, ld);
	}

	pageobj = pdf_array_get(ctx, dest, 0);
	if (pdf_is_int(ctx, pageobj))
	{
		pageno = pdf_to_int(ctx, pageobj);
		pageobj = pdf_lookup_page_obj(ctx, doc, pageno);
	}
	else
		pageno = pdf_lookup_page_number(ctx, doc, pageobj);

	destination.loc.chapter = 0;
	destination.loc.page = fz_clampi(pageno, 0, pdf_count_pages(ctx, doc) - 1);

	typeobj = pdf_array_get(ctx, dest, 1);
	if (typeobj == PDF_NAME(XYZ))
		destination.type = FZ_LINK_DEST_XYZ;
	else if (typeobj == PDF_NAME(Fit))
		destination.type = FZ_LINK_DEST_FIT;
	else if (typeobj == PDF_NAME(FitH))
		destination.type = FZ_LINK_DEST_FIT_H;
	else if (typeobj == PDF_NAME(FitV))
		destination.type = FZ_LINK_DEST_FIT_V;
	else if (typeobj == PDF_NAME(FitR))
		destination.type = FZ_LINK_DEST_FIT_R;
	else if (typeobj == PDF_NAME(FitB))
		destination.type = FZ_LINK_DEST_FIT_B;
	else if (typeobj == PDF_NAME(FitBH))
		destination.type = FZ_LINK_DEST_FIT_BH;
	else if (typeobj == PDF_NAME(FitBV))
		destination.type = FZ_LINK_DEST_FIT_BV;
	else
		destination.type = FZ_LINK_DEST_XYZ;

	arg1 = pdf_to_real(ctx, pdf_array_get(ctx, dest, 2));
	arg2 = pdf_to_real(ctx, pdf_array_get(ctx, dest, 3));
	arg3 = pdf_to_real(ctx, pdf_array_get(ctx, dest, 4));
	arg4 = pdf_to_real(ctx, pdf_array_get(ctx, dest, 5));

	pdf_page_obj_transform(ctx, pageobj, NULL, &page_ctm);

	switch (destination.type)
	{
	default:
	case FZ_LINK_DEST_FIT:
	case FZ_LINK_DEST_FIT_B:
		break;
	case FZ_LINK_DEST_FIT_H:
	case FZ_LINK_DEST_FIT_BH:
		p = fz_transform_point_xy(0, arg1, page_ctm);
		destination.y = p.y;
		break;
	case FZ_LINK_DEST_FIT_V:
	case FZ_LINK_DEST_FIT_BV:
		p = fz_transform_point_xy(arg1, 0, page_ctm);
		destination.x = p.x;
		break;
	case FZ_LINK_DEST_XYZ:
		p = fz_transform_point_xy(arg1, arg2, page_ctm);
		destination.x = p.x;
		destination.y = p.y;
		destination.zoom = arg3 * 100;
		break;
	case FZ_LINK_DEST_FIT_R:
		rect.x0 = arg1;
		rect.y0 = arg2;
		rect.x1 = arg3;
		rect.y1 = arg4;
		fz_transform_rect(rect, page_ctm);
		destination.x = rect.x0;
		destination.y = rect.y0;
		destination.w = rect.x1 - rect.x0;
		destination.h = rect.y1 - rect.y0;
		break;
	}

	return pdf_format_link_uri(ctx, destination);
		}

static char *
pdf_parse_file_spec(fz_context *ctx, pdf_document *doc, pdf_obj *file_spec, pdf_obj *dest)
{
	pdf_obj *filename = NULL;
	const char *path;
	char *uri;
	char frag[256];

	if (pdf_is_string(ctx, file_spec))
		filename = file_spec;

	if (pdf_is_dict(ctx, file_spec)) {
#ifdef _WIN32
		filename = pdf_dict_get(ctx, file_spec, PDF_NAME(DOS));
#else
		filename = pdf_dict_get(ctx, file_spec, PDF_NAME(Unix));
#endif
		if (!filename)
			filename = pdf_dict_geta(ctx, file_spec, PDF_NAME(UF), PDF_NAME(F));
	}

	if (!pdf_is_string(ctx, filename))
	{
		fz_warn(ctx, "cannot parse file specification");
		return NULL;
	}

	if (pdf_is_array(ctx, dest))
		fz_snprintf(frag, sizeof frag, "#page=%d", pdf_array_get_int(ctx, dest, 0) + 1);
	else if (pdf_is_name(ctx, dest))
		fz_snprintf(frag, sizeof frag, "#%s", pdf_to_name(ctx, dest));
	else if (pdf_is_string(ctx, dest))
		fz_snprintf(frag, sizeof frag, "#%s", pdf_to_str_buf(ctx, dest));
	else
		frag[0] = 0;

	path = pdf_to_text_string(ctx, filename);
	uri = NULL;
#ifdef _WIN32
	if (!pdf_name_eq(ctx, pdf_dict_get(ctx, file_spec, PDF_NAME(FS)), PDF_NAME(URL)))
	{
		/* Fix up the drive letter (change "/C/Documents/Foo" to "C:/Documents/Foo") */
		if (path[0] == '/' && (('A' <= path[1] && path[1] <= 'Z') || ('a' <= path[1] && path[1] <= 'z')) && path[2] == '/')
			uri = fz_asprintf(ctx, "file://%c:%s%s", path[1], path+2, frag);
	}
#endif
	if (!uri)
		uri = fz_asprintf(ctx, "file://%s%s", path, frag);

	return uri;
}

const char *
pdf_embedded_file_name(fz_context *ctx, pdf_obj *fs)
{
	pdf_obj *filename = pdf_dict_get(ctx, fs, PDF_NAME(UF));
	if (!filename) filename = pdf_dict_get(ctx, fs, PDF_NAME(F));
	if (!filename) filename = pdf_dict_get(ctx, fs, PDF_NAME(Unix));
	if (!filename) filename = pdf_dict_get(ctx, fs, PDF_NAME(DOS));
	if (!filename) filename = pdf_dict_get(ctx, fs, PDF_NAME(Mac));
	return pdf_to_text_string(ctx, filename);
}

pdf_obj *
pdf_embedded_file_stream(fz_context *ctx, pdf_obj *fs)
{
	pdf_obj *ef = pdf_dict_get(ctx, fs, PDF_NAME(EF));
	pdf_obj *file = pdf_dict_get(ctx, ef, PDF_NAME(UF));
	if (!file) file = pdf_dict_get(ctx, ef, PDF_NAME(F));
	if (!file) file = pdf_dict_get(ctx, ef, PDF_NAME(Unix));
	if (!file) file = pdf_dict_get(ctx, ef, PDF_NAME(DOS));
	if (!file) file = pdf_dict_get(ctx, ef, PDF_NAME(Mac));
	return file;
}

const char *
pdf_embedded_file_type(fz_context *ctx, pdf_obj *fs)
{
	pdf_obj *file = pdf_embedded_file_stream(ctx, fs);
	pdf_obj *subtype = pdf_dict_get(ctx, file, PDF_NAME(Subtype));
	return subtype ? pdf_to_name(ctx, subtype) : "application/octet-stream";
}

int
pdf_is_embedded_file(fz_context *ctx, pdf_obj *fs)
{
	return pdf_is_stream(ctx, pdf_embedded_file_stream(ctx, fs));
}

fz_buffer *
pdf_load_embedded_file(fz_context *ctx, pdf_obj *fs)
{
	return pdf_load_stream(ctx, pdf_embedded_file_stream(ctx, fs));
}

const char *
pdf_guess_mime_type_from_file_name(fz_context *ctx, const char *filename)
{
	const char *ext = strrchr(filename, '.');
	if (ext)
	{
		if (!fz_strcasecmp(ext, ".pdf")) return "application/pdf";
		if (!fz_strcasecmp(ext, ".xml")) return "application/xml";
		if (!fz_strcasecmp(ext, ".zip")) return "application/zip";
		if (!fz_strcasecmp(ext, ".tar")) return "application/x-tar";

		/* Text */
		if (!fz_strcasecmp(ext, ".txt")) return "text/plain";
		if (!fz_strcasecmp(ext, ".rtf")) return "application/rtf";
		if (!fz_strcasecmp(ext, ".csv")) return "text/csv";
		if (!fz_strcasecmp(ext, ".html")) return "text/html";
		if (!fz_strcasecmp(ext, ".htm")) return "text/html";
		if (!fz_strcasecmp(ext, ".css")) return "text/css";

		/* Office */
		if (!fz_strcasecmp(ext, ".doc")) return "application/msword";
		if (!fz_strcasecmp(ext, ".ppt")) return "application/vnd.ms-powerpoint";
		if (!fz_strcasecmp(ext, ".xls")) return "application/vnd.ms-excel";
		if (!fz_strcasecmp(ext, ".docx")) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
		if (!fz_strcasecmp(ext, ".pptx")) return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
		if (!fz_strcasecmp(ext, ".xlsx")) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
		if (!fz_strcasecmp(ext, ".odt")) return "application/vnd.oasis.opendocument.text";
		if (!fz_strcasecmp(ext, ".odp")) return "application/vnd.oasis.opendocument.presentation";
		if (!fz_strcasecmp(ext, ".ods")) return "application/vnd.oasis.opendocument.spreadsheet";

		/* Image */
		if (!fz_strcasecmp(ext, ".bmp")) return "image/bmp";
		if (!fz_strcasecmp(ext, ".gif")) return "image/gif";
		if (!fz_strcasecmp(ext, ".jpeg")) return "image/jpeg";
		if (!fz_strcasecmp(ext, ".jpg")) return "image/jpeg";
		if (!fz_strcasecmp(ext, ".png")) return "image/png";
		if (!fz_strcasecmp(ext, ".svg")) return "image/svg+xml";
		if (!fz_strcasecmp(ext, ".tif")) return "image/tiff";
		if (!fz_strcasecmp(ext, ".tiff")) return "image/tiff";

		/* Sound */
		if (!fz_strcasecmp(ext, ".flac")) return "audio/flac";
		if (!fz_strcasecmp(ext, ".mp3")) return "audio/mpeg";
		if (!fz_strcasecmp(ext, ".ogg")) return "audio/ogg";
		if (!fz_strcasecmp(ext, ".wav")) return "audio/wav";

		/* Movie */
		if (!fz_strcasecmp(ext, ".avi")) return "video/x-msvideo";
		if (!fz_strcasecmp(ext, ".mov")) return "video/quicktime";
		if (!fz_strcasecmp(ext, ".mp4")) return "video/mp4";
		if (!fz_strcasecmp(ext, ".webm")) return "video/webm";
	}
	return "application/octet-stream";
}

pdf_obj *
pdf_add_embedded_file(fz_context *ctx, pdf_document *doc,
	const char *filename, const char *mimetype, fz_buffer *contents)
{
	const char *s;
	char asciiname[1024];
	pdf_obj *file = NULL;
	pdf_obj *filespec = NULL;
	pdf_obj *ef;
	size_t i;

	fz_var(file);
	fz_var(filespec);

	for (i = 0, s = filename; *s && i + 1 < sizeof asciiname; ++i)
	{
		int c;
		s += fz_chartorune(&c, s);
		asciiname[i] = (c >= 32 && c <= 126) ? c : '_';
	}
	asciiname[i] = 0;

	if (!mimetype)
		mimetype = pdf_guess_mime_type_from_file_name(ctx, filename);

	pdf_begin_operation(ctx, doc, "Embed file");
	fz_try(ctx)
	{
		file = pdf_add_new_dict(ctx, doc, 3);
		pdf_dict_put(ctx, file, PDF_NAME(Type), PDF_NAME(EmbeddedFile));
		pdf_dict_put_name(ctx, file, PDF_NAME(Subtype), mimetype);
		pdf_update_stream(ctx, doc, file, contents, 0);

		filespec = pdf_add_new_dict(ctx, doc, 4);
		pdf_dict_put(ctx, filespec, PDF_NAME(Type), PDF_NAME(Filespec));
		pdf_dict_put_text_string(ctx, filespec, PDF_NAME(F), asciiname);
		pdf_dict_put_text_string(ctx, filespec, PDF_NAME(UF), filename);
		ef = pdf_dict_put_dict(ctx, filespec, PDF_NAME(EF), 1);
		pdf_dict_put(ctx, ef, PDF_NAME(F), file);
	}
	fz_always(ctx)
	{
		pdf_end_operation(ctx, doc);
		pdf_drop_obj(ctx, file);
	}
	fz_catch(ctx)
	{
		pdf_drop_obj(ctx, filespec);
		fz_rethrow(ctx);
	}

	return filespec;
}

char *
pdf_parse_link_action(fz_context *ctx, pdf_document *doc, pdf_obj *action, int pagenum)
{
	pdf_obj *obj, *dest, *file_spec;

	if (!action)
		return NULL;

	obj = pdf_dict_get(ctx, action, PDF_NAME(S));
	if (pdf_name_eq(ctx, PDF_NAME(GoTo), obj))
	{
		dest = pdf_dict_get(ctx, action, PDF_NAME(D));
		return pdf_parse_link_dest(ctx, doc, dest);
	}
	else if (pdf_name_eq(ctx, PDF_NAME(URI), obj))
	{
		/* URI entries are ASCII strings */
		const char *uri = pdf_dict_get_text_string(ctx, action, PDF_NAME(URI));
		if (!fz_is_external_link(ctx, uri))
		{
			pdf_obj *uri_base_obj = pdf_dict_getp(ctx, pdf_trailer(ctx, doc), "Root/URI/Base");
			const char *uri_base = uri_base_obj ? pdf_to_text_string(ctx, uri_base_obj) : "file://";
			char *new_uri = Memento_label(fz_malloc(ctx, strlen(uri_base) + strlen(uri) + 1), "link_action");
			strcpy(new_uri, uri_base);
			strcat(new_uri, uri);
			return new_uri;
		}
		return fz_strdup(ctx, uri);
	}
	else if (pdf_name_eq(ctx, PDF_NAME(Launch), obj))
	{
		file_spec = pdf_dict_get(ctx, action, PDF_NAME(F));
		return pdf_parse_file_spec(ctx, doc, file_spec, NULL);
	}
	else if (pdf_name_eq(ctx, PDF_NAME(GoToR), obj))
	{
		dest = pdf_dict_get(ctx, action, PDF_NAME(D));
		file_spec = pdf_dict_get(ctx, action, PDF_NAME(F));
		return pdf_parse_file_spec(ctx, doc, file_spec, dest);
	}
	else if (pdf_name_eq(ctx, PDF_NAME(Named), obj))
	{
		dest = pdf_dict_get(ctx, action, PDF_NAME(N));

		if (pdf_name_eq(ctx, PDF_NAME(FirstPage), dest))
			pagenum = 0;
		else if (pdf_name_eq(ctx, PDF_NAME(LastPage), dest))
			pagenum = pdf_count_pages(ctx, doc) - 1;
		else if (pdf_name_eq(ctx, PDF_NAME(PrevPage), dest) && pagenum >= 0)
		{
			if (pagenum > 0)
				pagenum--;
		}
		else if (pdf_name_eq(ctx, PDF_NAME(NextPage), dest) && pagenum >= 0)
		{
			if (pagenum < pdf_count_pages(ctx, doc) - 1)
				pagenum++;
		}
		else
			return NULL;

		return fz_asprintf(ctx, "#page=%d", pagenum + 1);
	}

	return NULL;
}

static fz_link *
pdf_load_link(fz_context *ctx, pdf_document *doc, pdf_obj *dict, int pagenum, fz_matrix page_ctm)
{
	pdf_obj *action;
	pdf_obj *obj;
	fz_rect bbox;
	char *uri;
	fz_link *link = NULL;

	obj = pdf_dict_get(ctx, dict, PDF_NAME(Subtype));
	if (!pdf_name_eq(ctx, obj, PDF_NAME(Link)))
		return NULL;

	obj = pdf_dict_get(ctx, dict, PDF_NAME(Rect));
	if (!obj)
		return NULL;

	bbox = pdf_to_rect(ctx, obj);
	bbox = fz_transform_rect(bbox, page_ctm);

	obj = pdf_dict_get(ctx, dict, PDF_NAME(Dest));
	if (obj)
		uri = pdf_parse_link_dest(ctx, doc, obj);
	else
	{
		action = pdf_dict_get(ctx, dict, PDF_NAME(A));
		/* fall back to additional action button's down/up action */
		if (!action)
			action = pdf_dict_geta(ctx, pdf_dict_get(ctx, dict, PDF_NAME(AA)), PDF_NAME(U), PDF_NAME(D));
		uri = pdf_parse_link_action(ctx, doc, action, pagenum);
	}

	if (!uri)
		return NULL;

	fz_try(ctx)
		link = fz_new_link(ctx, bbox, uri);
	fz_always(ctx)
		fz_free(ctx, uri);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return link;
}

fz_link *
pdf_load_link_annots(fz_context *ctx, pdf_document *doc, pdf_obj *annots, int pagenum, fz_matrix page_ctm)
{
	fz_link *link, *head, *tail;
	pdf_obj *obj;
	int i, n;

	head = tail = NULL;
	link = NULL;

	n = pdf_array_len(ctx, annots);
	for (i = 0; i < n; i++)
	{
		/* FIXME: Move the try/catch out of the loop for performance? */
		fz_try(ctx)
		{
			obj = pdf_array_get(ctx, annots, i);
			link = pdf_load_link(ctx, doc, obj, pagenum, page_ctm);
		}
		fz_catch(ctx)
		{
			fz_rethrow_if(ctx, FZ_ERROR_TRYLATER);
			link = NULL;
		}

		if (link)
		{
			if (!head)
				head = tail = link;
			else
			{
				tail->next = link;
				tail = link;
			}
		}
	}

	return head;
}

/* See explanation of this format in pdf-outline.c */
char *
pdf_format_link_uri(fz_context *ctx, fz_link_dest dest)
{
	char *uri = NULL;

	switch (dest.type)
		{
		default:
	case FZ_LINK_DEST_FIT:
		uri = fz_asprintf(ctx, "#page=%d&view=Fit", dest.loc.page + 1);
			break;
	case FZ_LINK_DEST_FIT_B:
		uri = fz_asprintf(ctx, "#page=%d&view=FitB", dest.loc.page + 1);
			break;
	case FZ_LINK_DEST_FIT_H:
		uri = fz_asprintf(ctx, "#page=%d&view=FitH,%g", dest.loc.page + 1, dest.y);
			break;
	case FZ_LINK_DEST_FIT_BH:
		uri = fz_asprintf(ctx, "#page=%d&view=FitBH,%g", dest.loc.page + 1, dest.y);
			break;
	case FZ_LINK_DEST_FIT_V:
		uri = fz_asprintf(ctx, "#page=%d&view=FitV,%g", dest.loc.page + 1, dest.x);
			break;
	case FZ_LINK_DEST_FIT_BV:
		uri = fz_asprintf(ctx, "#page=%d&view=FitBV,%g", dest.loc.page + 1, dest.x);
			break;
	case FZ_LINK_DEST_XYZ:
		if (dest.zoom == 0 && dest.x == 0 && dest.y == 0)
			uri = fz_asprintf(ctx, "#page=%d", dest.loc.page + 1);
		else
			uri = fz_asprintf(ctx, "#page=%d&zoom=%g,%g,%g", dest.loc.page + 1,
				dest.zoom,
				dest.x,
				dest.y);
			break;
	case FZ_LINK_DEST_FIT_R:
		uri = fz_asprintf(ctx, "#page=%d&viewrect=%g,%g,%g,%g", dest.loc.page + 1,
			dest.x,
			dest.y,
			dest.w,
			dest.h);
			break;
		}

	return uri;
}

static float next_float(const char *str, char **end)
{
	if (*str == ',')
		++str;
	return fz_strtof(str, end);
}

fz_link_dest
pdf_parse_link_uri(fz_context *ctx, const char *uri)
{
	fz_link_dest dest = fz_make_link_dest_xyz(0, 0, 0, 0, 0);
	char *page_s, *view_s, *rect_s, *zoom_s;

	if (!uri || uri[0] != '#')
	{
		fz_warn(ctx, "unknown link uri '%s'", uri);
		return dest;
	}

	page_s = strstr(uri, "page=");
	if (page_s)
		dest.loc.page = fz_atoi(page_s+5) - 1;

	rect_s = strstr(uri, "viewrect=");
	zoom_s = strstr(uri, "zoom=");
	view_s = strstr(uri, "view=");

	if (rect_s)
	{
		rect_s += 9;
		dest.type = FZ_LINK_DEST_FIT_R;
		dest.x = next_float(rect_s, &rect_s);
		dest.y = next_float(rect_s, &rect_s);
		dest.w = next_float(rect_s, &rect_s);
		dest.h = next_float(rect_s, &rect_s);
	}
	else if (zoom_s)
	{
		zoom_s += 5;
		dest.type = FZ_LINK_DEST_XYZ;
		dest.zoom = next_float(zoom_s, &zoom_s);
		dest.x = next_float(zoom_s, &zoom_s);
		dest.y = next_float(zoom_s, &zoom_s);
	}
	else if (view_s)
	{
		view_s += 5;
		if (!fz_strncasecmp(view_s, "FitH", 4))
		{
			view_s += 4;
			dest.type = FZ_LINK_DEST_FIT_H;
			dest.y = next_float(view_s, &view_s);
		}
		else if (!fz_strncasecmp(view_s, "FitBH", 5))
		{
			view_s += 5;
			dest.type = FZ_LINK_DEST_FIT_BH;
			dest.y = next_float(view_s, &view_s);
		}
		else if (!fz_strncasecmp(view_s, "FitV", 4))
		{
			view_s += 4;
			dest.type = FZ_LINK_DEST_FIT_V;
			dest.x = next_float(view_s, &view_s);
		}
		else if (!fz_strncasecmp(view_s, "FitBV", 5))
		{
			view_s += 5;
			dest.type = FZ_LINK_DEST_FIT_BV;
			dest.x = next_float(view_s, &view_s);
		}
		else if (!fz_strncasecmp(view_s, "FitB", 4))
		{
			dest.type = FZ_LINK_DEST_FIT_B;
		}
		else if (!fz_strncasecmp(view_s, "Fit", 3))
		{
			dest.type = FZ_LINK_DEST_FIT;
		}
	}

	return dest;
}
