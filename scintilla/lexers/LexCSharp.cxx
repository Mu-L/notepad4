// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
//! Lexer for C#, Vala.

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>
#include <vector>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "StringUtils.h"
#include "LexerModule.h"

using namespace Lexilla;

namespace {

constexpr bool HasEscapeChar(int state) noexcept {
	return state <= SCE_CSHARP_INTERPOLATED_STRING;
}

constexpr bool IsVerbatimString(int state) noexcept {
	return state >= SCE_CSHARP_VERBATIM_STRING;
}

constexpr bool IsInterpolatedString(int state) noexcept {
	if constexpr (SCE_CSHARP_INTERPOLATED_STRING & 1) {
		return state & true;
	} else {
		return (state & 1) == 0;
	}
}

constexpr bool IsSingleLineString(int state) noexcept {
	return state < SCE_CSHARP_RAWSTRING_ML;
}

constexpr bool IsPlainString(int state) noexcept {
	return state < SCE_CSHARP_RAWSTRING_SL || state > SCE_CSHARP_INTERPOLATED_RAWSTRING_ML;
}

struct EscapeSequence {
	int outerState = SCE_CSHARP_DEFAULT;
	int digitsLeft = 0;

	// highlight any character as escape sequence.
	bool resetEscapeState(int state, int chNext) noexcept {
		if (IsEOLChar(chNext)) {
			return false;
		}
		outerState = state;
		digitsLeft = 1;
		if (chNext == 'x' || chNext == 'u') {
			digitsLeft = 5;
		} else if (chNext == 'U') {
			digitsLeft = 9;
		}
		return true;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsHexDigit(ch);
	}
};

struct InterpolatedStringState {
	int state;
	int parenCount;
	int delimiterCount;
	int interpolatorCount;
};

enum {
	CSharpLineStateMaskLineComment = 1,
	CSharpLineStateMaskUsing = 1 << 1,
	CSharpLineStateMaskInterpolation = 1 << 2,
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_Type = 1,
	KeywordIndex_ValaType = 2,
	KeywordIndex_Preprocessor = 3,
	KeywordIndex_Attribute = 4,
	KeywordIndex_Class = 5,
	KeywordIndex_Struct = 6,
	KeywordIndex_Interface = 7,
	KeywordIndex_Enumeration = 8,
	KeywordIndex_Constant = 9,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

enum class PreprocessorKind {
	None,
	Init,
	Pragma,
	Message,
	Other,
};

enum class DocTagState {
	None,
	XmlOpen,
	XmlClose,
};

enum class KeywordType {
	None = SCE_CSHARP_DEFAULT,
	Attribute = SCE_CSHARP_ATTRIBUTE,
	Class = SCE_CSHARP_CLASS,
	Interface = SCE_CSHARP_INTERFACE,
	Struct = SCE_CSHARP_STRUCT,
	Enum = SCE_CSHARP_ENUM,
	Record = SCE_CSHARP_RECORD,
	Label = SCE_CSHARP_LABEL,
	Return = 0x40,
	While = 0x41,
};

constexpr bool IsUnicodeEscape(int ch, int chNext) noexcept {
	return ch == '\\' && UnsafeLower(chNext) == 'u';
}

constexpr bool IsCsIdentifierStart(int ch, int chNext) noexcept {
	return IsIdentifierStartEx(ch) || IsUnicodeEscape(ch, chNext);
}

constexpr bool IsCsIdentifierChar(int ch, int chNext) noexcept {
	return IsIdentifierCharEx(ch) || IsUnicodeEscape(ch, chNext);
}

constexpr bool IsXmlCommentTagChar(int ch) noexcept {
	return IsIdentifierChar(ch) || ch == '-' || ch ==':';
}

constexpr bool PreferArrayIndex(int ch) noexcept {
	return ch == ')' || ch == ']' || IsIdentifierCharEx(ch);
}

constexpr bool IsSpaceEquiv(int state) noexcept {
	return state <= SCE_CSHARP_TASKMARKER;
}

// https://docs.microsoft.com/en-us/dotnet/standard/base-types/composite-formatting
constexpr bool IsInvalidFormatSpecifier(int ch) noexcept {
	// Custom format strings allows any characters
	return (ch >= '\0' && ch < ' ') || ch == '\"' || ch == '{' || ch == '}';
}

inline Sci_Position CheckFormatSpecifier(const StyleContext &sc, LexAccessor &styler) noexcept {
	Sci_PositionU pos = sc.currentPos;
	char ch = styler[pos];
	// [,alignment]
	if (ch == ',') {
		ch = styler[++pos];
		if (ch == '-') {
			ch = styler[++pos];
		}
		while (IsADigit(ch)) {
			ch = styler[++pos];
		}
	}
	// [:formatString]
	if (ch == ':') {
		ch = styler[++pos];
		const Sci_PositionU endPos = pos + 32;
		while (pos < endPos && !IsInvalidFormatSpecifier(ch)) {
			ch = styler[++pos];
		}
	}
	if (ch == '}') {
		return pos - sc.currentPos;
	}
	return 0;
}

inline bool IsInterpolatedStringEnd(const StyleContext &sc) noexcept {
	return sc.ch == '}' || sc.ch == ':'
		|| (sc.ch == ',' && (IsADigit(sc.chNext) || (sc.chNext == '-' && IsADigit(sc.GetRelative(2)))));
}

void ColouriseCSharpDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	int lineStateLineType = 0;

	KeywordType kwType = KeywordType::None;
	int chBeforeIdentifier = 0;
	int parenCount = 0;
	int stringDelimiterCount = 0;
	int stringInterpolatorCount = 0;
	PreprocessorKind ppKind = PreprocessorKind::None;

	int visibleChars = 0;
	int chBefore = 0;
	int visibleCharsBefore = 0;
	int chPrevNonWhite = 0;
	DocTagState docTagState = DocTagState::None;
	EscapeSequence escSeq;
	bool closeBrace = false;

	std::vector<InterpolatedStringState> nestedState;

	if (startPos != 0) {
		// backtrack to the line starts expression inside interpolated string literal.
		BacktrackToStart(styler, CSharpLineStateMaskInterpolation, startPos, lengthDoc, initStyle);
	}

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		const int lineState = styler.GetLineState(sc.currentLine - 1);
		/*
		1: CSharpLineStateMaskLineComment
		1: CSharpLineStateMaskUsing
		1: CSharpLineStateMaskInterpolation
		1: unused
		8: stringDelimiterCount
		8: stringInterpolatorCount
		12: parenCount
		*/
		stringDelimiterCount = (lineState >> 4) & 0xff;
		stringInterpolatorCount = (lineState >> 12) & 0xff;
		parenCount = lineState >> 20;
	}
	if (startPos == 0) {
		if (sc.Match('#', '!')) {
			// Shell Shebang at beginning of file
			sc.SetState(SCE_CSHARP_COMMENTLINE);
			sc.Forward();
			lineStateLineType = CSharpLineStateMaskLineComment;
		}
	} else if (IsSpaceEquiv(initStyle)) {
		LookbackNonWhite(styler, startPos, SCE_CSHARP_TASKMARKER, chPrevNonWhite, initStyle);
	}

	while (sc.More()) {
		switch (sc.state) {
		case SCE_CSHARP_OPERATOR:
		case SCE_CSHARP_OPERATOR2:
			sc.SetState(SCE_CSHARP_DEFAULT);
			break;

		case SCE_CSHARP_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_CSHARP_DEFAULT);
			}
			break;

		case SCE_CSHARP_IDENTIFIER:
		case SCE_CSHARP_PREPROCESSOR:
			if (!IsCsIdentifierChar(sc.ch, sc.chNext)) {
				char s[128];
				sc.GetCurrent(s, sizeof(s));
				switch (ppKind) {
				case PreprocessorKind::None:
					if (s[0] != '@') {
						if (keywordLists[KeywordIndex_Keyword].InList(s)) {
							sc.ChangeState(SCE_CSHARP_WORD);
							if (StrEqual(s, "using")) {
								if (visibleChars == sc.LengthCurrent()) {
									lineStateLineType = CSharpLineStateMaskUsing;
								}
							} else if (StrEqualsAny(s, "class", "new", "as", "is")) {
								kwType = KeywordType::Class;
							} else if (StrEqual(s, "struct")) {
								kwType = KeywordType::Struct;
							} else if (StrEqual(s, "interface")) {
								kwType = KeywordType::Interface;
							} else if (StrEqual(s, "enum")) {
								kwType = KeywordType::Enum;
							} else if (StrEqual(s, "record")) {
								kwType = KeywordType::Record;
							} else if (StrEqual(s, "goto")) {
								kwType = KeywordType::Label;
							} else if (StrEqualsAny(s, "return", "await", "yield")) {
								kwType = KeywordType::Return;
							} else if (StrEqualsAny(s, "if", "while")) {
								// to avoid treating following code as type cast:
								// if (identifier) expression, while (identifier) expression
								kwType = KeywordType::While;
							}
							if (kwType > KeywordType::None && kwType < KeywordType::Return) {
								const int chNext = sc.GetDocNextChar();
								if (!IsIdentifierStartEx(chNext)) {
									kwType = KeywordType::None;
								}
							}
						} else if (keywordLists[KeywordIndex_Type].InList(s) || keywordLists[KeywordIndex_ValaType].InList(s)) {
							sc.ChangeState(SCE_CSHARP_WORD2);
						} else if (keywordLists[KeywordIndex_Class].InList(s)) {
							sc.ChangeState(SCE_CSHARP_CLASS);
						} else if (keywordLists[KeywordIndex_Struct].InList(s)) {
							sc.ChangeState(SCE_CSHARP_STRUCT);
						} else if (keywordLists[KeywordIndex_Interface].InList(s)) {
							sc.ChangeState(SCE_CSHARP_INTERFACE);
						} else if (keywordLists[KeywordIndex_Enumeration].InList(s)) {
							sc.ChangeState(SCE_CSHARP_ENUM);
						} else if (keywordLists[KeywordIndex_Attribute].InList(s)) {
							sc.ChangeState(SCE_CSHARP_ATTRIBUTE);
						} else if (keywordLists[KeywordIndex_Constant].InList(s)) {
							sc.ChangeState(SCE_CSHARP_CONSTANT);
						}
					}
					break;

				case PreprocessorKind::Init:
					if (sc.state == SCE_CSHARP_IDENTIFIER) {
						sc.ChangeState(SCE_CSHARP_PREPROCESSOR);
					}
					if (sc.LengthCurrent() > 1) {
						const char *p = s;
						if (*p == '#') {
							++p;
						}
						if (StrEqualsAny(p, "pragma", "line", "nullable")) {
							ppKind = PreprocessorKind::Pragma;
						} else if (StrEqualsAny(p, "error", "warning", "region", "endregion")) {
							ppKind = PreprocessorKind::Message;
						} else {
							ppKind = PreprocessorKind::Other;
						}
					} else if (!IsASpaceOrTab(sc.ch)) {
						ppKind = PreprocessorKind::Other;
					}
					break;

				case PreprocessorKind::Pragma:
					ppKind = PreprocessorKind::Other;
					sc.ChangeState(SCE_CSHARP_PREPROCESSOR_WORD);
					break;

				default:
					break;
				}

				if (ppKind == PreprocessorKind::None && sc.state == SCE_CSHARP_IDENTIFIER) {
					if (sc.ch == ':') {
						if (parenCount == 0 && IsJumpLabelPrevChar(chBefore)) {
							sc.ChangeState(SCE_CSHARP_LABEL);
						} else if (chBefore == '[') {
							// [target: Attribute]
							sc.ChangeState(SCE_CSHARP_ATTRIBUTE);
							kwType = KeywordType::Attribute;
						}
					} else if (sc.ch != '.') {
						if (kwType > KeywordType::None && kwType < KeywordType::Return) {
							sc.ChangeState(static_cast<int>(kwType));
						} else {
							const int chNext = sc.GetDocNextChar(sc.ch == '?' || sc.ch == ')');
							if (sc.ch == ')') {
								if (chBeforeIdentifier == '(' && (chNext == '(' || (kwType != KeywordType::While && IsIdentifierCharEx(chNext)))) {
									// (type)(expression)
									// (type)expression, (type)++identifier, (type)--identifier
									sc.ChangeState(SCE_CSHARP_CLASS);
								}
							} else if (chNext == '(') {
								if (kwType != KeywordType::Return && (IsIdentifierCharEx(chBefore) || chBefore == ']')) {
									// type method()
									// type[] method()
									// type<type> method()
									sc.ChangeState(SCE_CSHARP_FUNCTION_DEFINITION);
								} else {
									sc.ChangeState(SCE_CSHARP_FUNCTION);
								}
							} else if ((sc.ch == '[' && (sc.chNext == ']' || sc.chNext == ','))
								|| (chBeforeIdentifier == '<' && (chNext == '>' || chNext == '<'))
								|| IsIdentifierStartEx(chNext)) {
								// type[] identifier
								// type[,] identifier
								// type<type>
								// type<type?>
								// type<type<type>>
								// type<type, type>
								// class type: type, interface {}
								// type identifier
								sc.ChangeState(IsInterfaceName(s[0], s[1]) ? SCE_CSHARP_INTERFACE : SCE_CSHARP_CLASS);
							}
						}
#if 0
						if (sc.state == SCE_CSHARP_IDENTIFIER && IsInterfaceName(s[0], s[1])) {
							sc.ChangeState(SCE_CSHARP_INTERFACE);
						}
#endif
					}
				}
				if (sc.state != SCE_CSHARP_WORD && sc.state != SCE_CSHARP_ATTRIBUTE && sc.ch != '.') {
					kwType = KeywordType::None;
				}
				sc.SetState(SCE_CSHARP_DEFAULT);
			}
			break;

		case SCE_CSHARP_PREPROCESSOR_MESSAGE:
			if (sc.atLineStart) {
				sc.SetState(SCE_CSHARP_DEFAULT);
			}
			break;

		case SCE_CSHARP_COMMENTLINE:
		case SCE_CSHARP_COMMENTLINEDOC:
		case SCE_CSHARP_COMMENTBLOCK:
		case SCE_CSHARP_COMMENTBLOCKDOC:
			if (sc.atLineStart && (sc.state == SCE_CSHARP_COMMENTLINE || sc.state == SCE_CSHARP_COMMENTLINEDOC)) {
				sc.SetState(SCE_CSHARP_DEFAULT);
				break;
			}
			if (docTagState != DocTagState::None) {
				if (sc.Match('/', '>') || sc.ch == '>') {
					docTagState = DocTagState::None;
					sc.SetState(SCE_CSHARP_COMMENTTAG_XML);
					sc.Forward((sc.ch == '/') ? 2 : 1);
					sc.SetState(escSeq.outerState);
				}
			}
			if ((sc.state == SCE_CSHARP_COMMENTBLOCK || sc.state == SCE_CSHARP_COMMENTBLOCKDOC) && sc.Match('*', '/')) {
				sc.Forward();
				sc.ForwardSetState(SCE_CSHARP_DEFAULT);
				break;
			}
			if (docTagState == DocTagState::None) {
				if (sc.ch == '<' && (sc.state == SCE_CSHARP_COMMENTLINEDOC || sc.state == SCE_CSHARP_COMMENTBLOCKDOC)) {
					if (IsAlpha(sc.chNext)) {
						docTagState = DocTagState::XmlOpen;
						escSeq.outerState = sc.state;
						sc.SetState(SCE_CSHARP_COMMENTTAG_XML);
					} else if (sc.chNext == '/' && IsAlpha(sc.GetRelative(2))) {
						docTagState = DocTagState::XmlClose;
						escSeq.outerState = sc.state;
						sc.SetState(SCE_CSHARP_COMMENTTAG_XML);
						sc.Forward();
					}
				} else if (HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_CSHARP_TASKMARKER)) {
					continue;
				}
			}
			break;

		case SCE_CSHARP_COMMENTTAG_XML:
			if (!IsXmlCommentTagChar(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_CSHARP_CHARACTER:
		case SCE_CSHARP_STRING:
		case SCE_CSHARP_INTERPOLATED_STRING:
		case SCE_CSHARP_VERBATIM_STRING:
		case SCE_CSHARP_INTERPOLATED_VERBATIM_STRING:
		case SCE_CSHARP_RAWSTRING_SL:
		case SCE_CSHARP_INTERPOLATED_RAWSTRING_SL:
		case SCE_CSHARP_RAWSTRING_ML:
		case SCE_CSHARP_INTERPOLATED_RAWSTRING_ML:
			if (sc.atLineStart && IsSingleLineString(sc.state)) {
				if (!closeBrace) {
					sc.SetState(SCE_CSHARP_DEFAULT);
					break;
				}
			}
			if  (sc.ch == '\\') {
				if (HasEscapeChar(sc.state)) {
					if (escSeq.resetEscapeState(sc.state, sc.chNext)) {
						sc.SetState(SCE_CSHARP_ESCAPECHAR);
						sc.Forward();
					}
				}
			} else if (sc.ch == '\'' && sc.state == SCE_CSHARP_CHARACTER) {
				sc.ForwardSetState(SCE_CSHARP_DEFAULT);
			} else if (sc.state != SCE_CSHARP_CHARACTER) {
				if (sc.ch == '\"') {
					if (sc.chNext == '\"' && IsVerbatimString(sc.state)) {
						escSeq.outerState = sc.state;
						escSeq.digitsLeft = 1;
						sc.SetState(SCE_CSHARP_ESCAPECHAR);
						sc.Forward();
					} else {
						sc.Forward();
						bool handled = IsPlainString(sc.state);
						if (!handled && sc.Match('\"', '\"') && (visibleChars == 0 || IsSingleLineString(sc.state))) {
							const int delimiterCount = GetMatchedDelimiterCount(styler, sc.currentPos + 1, '\"') + 2;
							if (delimiterCount == stringDelimiterCount) {
								handled = true;
								stringDelimiterCount = 0;
								stringInterpolatorCount = 0;
								sc.Advance(delimiterCount - 1);
							}
						}
						if (handled) {
							if (sc.chNext == '8' && UnsafeLower(sc.ch) == 'u') {
								sc.Forward(2); // C# 11 UTF-8 string literal
							}
							sc.SetState(SCE_CSHARP_DEFAULT);
							if (!nestedState.empty() && nestedState.back().state == sc.state) {
								nestedState.pop_back();
							}
						} else {
							continue;
						}
					}
				} else if (sc.ch == '{') {
					if (sc.chNext == '{' && IsPlainString(sc.state)) {
						escSeq.outerState = sc.state;
						escSeq.digitsLeft = 1;
						sc.SetState(SCE_CSHARP_ESCAPECHAR);
						sc.Forward();
						break;
					}
					if (IsInterpolatedString(sc.state)) {
						const int interpolatorCount = GetMatchedDelimiterCount(styler, sc.currentPos, '{');
						if (IsPlainString(sc.state) || interpolatorCount >= stringInterpolatorCount) {
							nestedState.push_back({sc.state, 0, stringDelimiterCount, stringInterpolatorCount});
							sc.Advance(interpolatorCount - stringInterpolatorCount); // outer content
							sc.SetState(SCE_CSHARP_OPERATOR2);
							sc.Advance(stringInterpolatorCount - 1); // inner interpolation
							sc.ForwardSetState(SCE_CSHARP_DEFAULT);
							stringDelimiterCount = 0;
							stringInterpolatorCount = 0;
							break;
						}
					}
					if (IsIdentifierCharEx(sc.chNext) || sc.chNext == '@' || sc.chNext == '$') {
						// standard format: {index,alignment:format}
						// third party string template library: {@identifier} {$identifier} {identifier}
						escSeq.outerState = sc.state;
						sc.SetState(SCE_CSHARP_PLACEHOLDER);
						if (sc.chNext == '@' || sc.chNext == '$') {
							sc.Forward();
						}
					}
				} else if (sc.ch == '}') {
					closeBrace = false;
					if (IsInterpolatedString(sc.state)) {
						const int interpolatorCount = IsPlainString(sc.state) ? 1 : GetMatchedDelimiterCount(styler, sc.currentPos, '}');
						const bool interpolating = !nestedState.empty() && (interpolatorCount >= stringInterpolatorCount);
						if (interpolating) {
							nestedState.pop_back();
						}
						if (interpolating || (sc.chNext != '}' && IsPlainString(sc.state))) {
							const int state = sc.state;
							sc.SetState(SCE_CSHARP_OPERATOR2);
							sc.Advance(stringInterpolatorCount - 1); // inner interpolation
							sc.ForwardSetState(state);
							sc.Advance(interpolatorCount - stringInterpolatorCount); // outer content
							continue;
						}
					}
					if (sc.chNext == '}' && IsPlainString(sc.state)) {
						escSeq.outerState = sc.state;
						escSeq.digitsLeft = 1;
						sc.SetState(SCE_CSHARP_ESCAPECHAR);
						sc.Forward();
					}
				}
			}
			break;

		case SCE_CSHARP_FORMAT_SPECIFIER:
			if (IsInvalidFormatSpecifier(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_CSHARP_PLACEHOLDER:
			if (!IsIdentifierCharEx(sc.ch)) {
				if (sc.ch != '}') {
					const Sci_Position length = CheckFormatSpecifier(sc, styler);
					if (length == 0) {
						sc.Rewind();
						sc.ChangeState(escSeq.outerState);
					} else {
						sc.SetState(SCE_CSHARP_FORMAT_SPECIFIER);
						sc.Advance(length);
						sc.SetState(SCE_CSHARP_PLACEHOLDER);
					}
				}
				sc.ForwardSetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_CSHARP_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;
		}

		if (sc.state == SCE_CSHARP_DEFAULT) {
			if (ppKind == PreprocessorKind::Message && !isspacechar(sc.ch)) {
				sc.SetState(SCE_CSHARP_PREPROCESSOR_MESSAGE);
			} else if (sc.ch == '/' && (sc.chNext == '/' || sc.chNext == '*')) {
				visibleCharsBefore = visibleChars;
				docTagState = DocTagState::None;
				const int chNext = sc.chNext;
				if (chNext == '/' && visibleChars == 0) {
					lineStateLineType = CSharpLineStateMaskLineComment;
				}
				sc.SetState((chNext == '/') ? SCE_CSHARP_COMMENTLINE : SCE_CSHARP_COMMENTBLOCK);
				sc.Forward(2);
				if (sc.ch == chNext && sc.chNext != chNext) {
					static_assert(SCE_CSHARP_COMMENTLINEDOC - SCE_CSHARP_COMMENTLINE == SCE_CSHARP_COMMENTBLOCKDOC - SCE_CSHARP_COMMENTBLOCK);
					sc.ChangeState(sc.state + SCE_CSHARP_COMMENTLINEDOC - SCE_CSHARP_COMMENTLINE);
				}
				continue;
			} else if (sc.ch == '\"' || sc.ch == '$' || sc.ch == '@') {
				int chNext = sc.GetRelative(2);
				// C# 8 verbatim interpolated string: @$""
				if (chNext == '\"' && (sc.Match('$', '@') || sc.Match('@', '$'))) {
					stringDelimiterCount = 0;
					stringInterpolatorCount = 1;
					sc.SetState(SCE_CSHARP_INTERPOLATED_VERBATIM_STRING);
					sc.Advance(2);
				} else if (sc.ch == '@') {
					int state = SCE_CSHARP_DEFAULT;
					if (sc.chNext == '\"') {
						state = SCE_CSHARP_VERBATIM_STRING;
						stringDelimiterCount = 0;
						stringInterpolatorCount = 0;
					} else if (IsCsIdentifierStart(sc.chNext, chNext)) {
						state = SCE_CSHARP_IDENTIFIER;
						chBefore = chPrevNonWhite;
						if (chPrevNonWhite != '.') {
							chBeforeIdentifier = chPrevNonWhite;
						}
					}
					if (state != SCE_CSHARP_DEFAULT) {
						sc.SetState(state);
						sc.Forward();
					}
				} else {
					int interpolatorCount = 0;
					Sci_PositionU pos = sc.currentPos;
					chNext = sc.ch;
					if (chNext == '$') {
						interpolatorCount = 1;
						if (sc.chNext == '\"') {
							chNext = '\"';
							pos += 1;
						} else if (sc.chNext == '$') {
							interpolatorCount += GetMatchedDelimiterCount(styler, pos + 1, '$');
							pos += interpolatorCount;
							chNext = static_cast<uint8_t>(styler[pos]);
						}
					}
					if (chNext == '\"') {
						int delimiterCount = GetMatchedDelimiterCount(styler, pos, '\"');
						int state;
						if (delimiterCount >= 3) {
							chNext = LexGetNextChar(styler, pos + delimiterCount, sc.lineStartNext);
							stringDelimiterCount = delimiterCount;
							stringInterpolatorCount = interpolatorCount;
							state = (chNext == '\0') ? SCE_CSHARP_RAWSTRING_ML : SCE_CSHARP_RAWSTRING_SL;
							if (interpolatorCount) {
								delimiterCount += interpolatorCount;
								state += SCE_CSHARP_INTERPOLATED_RAWSTRING_SL - SCE_CSHARP_RAWSTRING_SL;
							}
						} else {
							delimiterCount = 1 + interpolatorCount;
							stringDelimiterCount = 0;
							stringInterpolatorCount = interpolatorCount;
							state = interpolatorCount + SCE_CSHARP_STRING;
						}
						sc.SetState(state);
						sc.Advance(delimiterCount - 1);
					}
				}
			} else if (sc.ch == '\'') {
				sc.SetState(SCE_CSHARP_CHARACTER);
			} else if (visibleChars == 0 && sc.ch == '#') {
				ppKind = PreprocessorKind::Init;
				sc.SetState(SCE_CSHARP_PREPROCESSOR);
			} else if (IsNumberStart(sc.ch, sc.chNext)) {
				sc.SetState(SCE_CSHARP_NUMBER);
			} else if (IsCsIdentifierStart(sc.ch, sc.chNext)) {
				chBefore = chPrevNonWhite;
				if (chPrevNonWhite != '.') {
					chBeforeIdentifier = chPrevNonWhite;
				}
				sc.SetState(SCE_CSHARP_IDENTIFIER);
			} else if (IsAGraphic(sc.ch) && sc.ch != '\\') {
				const bool interpolating = !nestedState.empty();
				sc.SetState(interpolating ? SCE_CSHARP_OPERATOR2 : SCE_CSHARP_OPERATOR);
				if (sc.ch == '(' || sc.ch == '[') {
					if (interpolating) {
						nestedState.back().parenCount += 1;
					} else {
						++parenCount;
					}
				} else if (sc.ch == ')' || sc.ch == ']') {
					if (interpolating) {
						InterpolatedStringState &state = nestedState.back();
						--state.parenCount;
					} else {
						if (parenCount > 0) {
							--parenCount;
						}
					}
				}
				if (interpolating) {
					const InterpolatedStringState &state = nestedState.back();
					if (state.parenCount <= 0 && IsInterpolatedStringEnd(sc)) {
						escSeq.outerState = state.state;
						stringDelimiterCount = state.delimiterCount;
						stringInterpolatorCount = state.interpolatorCount;
						closeBrace = sc.ch == '}';
						sc.ChangeState(closeBrace ? state.state : SCE_CSHARP_FORMAT_SPECIFIER);
						continue;
					}
				} else {
					if (kwType == KeywordType::None && sc.ch == '[') {
						if (visibleChars == 0 || !PreferArrayIndex(chPrevNonWhite)) {
							kwType = KeywordType::Attribute;
						}
					} else if (kwType == KeywordType::Attribute && (sc.ch == '(' || sc.ch == ']')) {
						kwType = KeywordType::None;
					}
				}
			}
		}

		if (!isspacechar(sc.ch)) {
			visibleChars++;
			if (!IsSpaceEquiv(sc.state)) {
				chPrevNonWhite = sc.ch;
			}
		}
		if (sc.atLineEnd) {
			uint32_t lineState = lineStateLineType
				| (static_cast<uint32_t>(stringDelimiterCount) << 4)
				| (static_cast<uint32_t>(stringInterpolatorCount) << 12)
				| (static_cast<uint32_t>(parenCount) << 20);
			if (!nestedState.empty()) {
				// C# 11 allows new line
				lineState |= CSharpLineStateMaskInterpolation;
			}
			styler.SetLineState(sc.currentLine, static_cast<int>(lineState));
			lineStateLineType = 0;
			visibleChars = 0;
			visibleCharsBefore = 0;
			docTagState = DocTagState::None;
			ppKind = PreprocessorKind::None;
			kwType = KeywordType::None;
		}
		sc.Forward();
	}

	sc.Complete();
}

struct FoldLineState {
	int lineComment;
	int usingName;
	constexpr explicit FoldLineState(int lineState) noexcept:
		lineComment(lineState & CSharpLineStateMaskLineComment),
		usingName((lineState >> 1) & 1) {
	}
};

void FoldCSharpDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList /*keywordLists*/, Accessor &styler) {
	const Sci_PositionU endPos = startPos + lengthDoc;
	Sci_Line lineCurrent = styler.GetLine(startPos);
	FoldLineState foldPrev(0);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0) {
		levelCurrent = styler.LevelAt(lineCurrent - 1) >> 16;
		foldPrev = FoldLineState(styler.GetLineState(lineCurrent - 1));
		const Sci_PositionU bracePos = CheckBraceOnNextLine(styler, lineCurrent - 1, SCE_CSHARP_OPERATOR, SCE_CSHARP_TASKMARKER, SCE_CSHARP_PREPROCESSOR);
		if (bracePos) {
			startPos = bracePos + 1; // skip the brace
		}
	}

	int levelNext = levelCurrent;
	FoldLineState foldCurrent(styler.GetLineState(lineCurrent));
	Sci_PositionU lineStartNext = styler.LineStart(lineCurrent + 1);
	lineStartNext = sci::min(lineStartNext, endPos);

	char buf[12]; // endregion
	constexpr int MaxFoldWordLength = sizeof(buf) - 1;
	int wordLen = 0;

	int styleNext = styler.StyleAt(startPos);
	int style = initStyle;
	int visibleChars = 0;

	while (startPos < endPos) {
		const int stylePrev = style;
		style = styleNext;
		styleNext = styler.StyleAt(startPos + 1);

		switch (style) {
		case SCE_CSHARP_COMMENTBLOCK:
		case SCE_CSHARP_COMMENTBLOCKDOC:
		case SCE_CSHARP_VERBATIM_STRING:
		case SCE_CSHARP_INTERPOLATED_VERBATIM_STRING:
		case SCE_CSHARP_RAWSTRING_ML:
		case SCE_CSHARP_INTERPOLATED_RAWSTRING_ML:
			if (style != stylePrev) {
				levelNext++;
			}
			if (style != styleNext) {
				levelNext--;
			}
			break;

		case SCE_CSHARP_OPERATOR:
		case SCE_CSHARP_OPERATOR2: {
			const char ch = styler[startPos];
			if (ch == '{' || ch == '[' || ch == '(') {
				levelNext++;
			} else if (ch == '}' || ch == ']' || ch == ')') {
				levelNext--;
			}
		} break;

		case SCE_CSHARP_PREPROCESSOR:
			if (wordLen < MaxFoldWordLength) {
				buf[wordLen++] = styler[startPos];
			}
			if (styleNext != style) {
				buf[wordLen] = '\0';
				wordLen = 0;
				const char *p = buf;
				if (*p == '#') {
					++p;
				}
				if (StrEqualsAny(p, "if", "region")) {
					levelNext++;
				} else if (StrStartsWith(p, "end")) {
					levelNext--;
				}
			}
			break;
		}

		if (visibleChars == 0 && !IsSpaceEquiv(style)) {
			++visibleChars;
		}
		if (++startPos == lineStartNext) {
			const FoldLineState foldNext(styler.GetLineState(lineCurrent + 1));
			levelNext = sci::max(levelNext, SC_FOLDLEVELBASE);
			if (foldCurrent.lineComment) {
				levelNext += foldNext.lineComment - foldPrev.lineComment;
			} else if (foldCurrent.usingName) {
				levelNext += foldNext.usingName - foldPrev.usingName;
			} else if (visibleChars) {
				const Sci_PositionU bracePos = CheckBraceOnNextLine(styler, lineCurrent, SCE_CSHARP_OPERATOR, SCE_CSHARP_TASKMARKER, SCE_CSHARP_PREPROCESSOR);
				if (bracePos) {
					levelNext++;
					startPos = bracePos + 1; // skip the brace
					style = SCE_CSHARP_OPERATOR;
					styleNext = styler.StyleAt(startPos);
				}
			}

			const int levelUse = levelCurrent;
			int lev = levelUse | (levelNext << 16);
			if (levelUse < levelNext) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			}
			styler.SetLevel(lineCurrent, lev);

			lineCurrent++;
			lineStartNext = styler.LineStart(lineCurrent + 1);
			lineStartNext = sci::min(lineStartNext, endPos);
			levelCurrent = levelNext;
			foldPrev = foldCurrent;
			foldCurrent = foldNext;
			visibleChars = 0;
		}
	}
}

}

LexerModule lmCSharp(SCLEX_CSHARP, ColouriseCSharpDoc, "csharp", FoldCSharpDoc);
