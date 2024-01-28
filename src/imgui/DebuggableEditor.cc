#include "DebuggableEditor.hh"

#include "ImGuiCpp.hh"
#include "ImGuiManager.hh"
#include "ImGuiUtils.hh"

#include "CommandException.hh"
#include "Debuggable.hh"
#include "Debugger.hh"
#include "Interpreter.hh"
#include "MSXMotherBoard.hh"
#include "SymbolManager.hh"
#include "TclObject.hh"

#include "narrow.hh"
#include "unreachable.hh"

#include "imgui_stdlib.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>

namespace openmsx {

using namespace std::literals;

static constexpr int MidColsCount = 8; // extra spacing between every mid-cols.
static constexpr auto HighlightColor = IM_COL32(255, 255, 255, 50); // background color of highlighted bytes.

DebuggableEditor::DebuggableEditor(ImGuiManager& manager_, std::string debuggableName_, size_t index)
	: ImGuiPart(manager_)
	, symbolManager(manager.getReactor().getSymbolManager())
	, title(std::move(debuggableName_))
{
	debuggableNameSize = title.size();
	if (index) {
		strAppend(title, " (", index + 1, ')');
	}
}

void DebuggableEditor::save(ImGuiTextBuffer& buf)
{
	savePersistent(buf, *this, persistentElements);
}

void DebuggableEditor::loadLine(std::string_view name, zstring_view value)
{
	loadOnePersistent(name, value, *this, persistentElements);
}

void DebuggableEditor::loadEnd()
{
	updateAddr = true;
}

DebuggableEditor::Sizes DebuggableEditor::calcSizes(unsigned memSize)
{
	Sizes s;
	const auto& style = ImGui::GetStyle();

	s.addrDigitsCount = 0;
	for (unsigned n = memSize - 1; n > 0; n >>= 4) {
		++s.addrDigitsCount;
	}

	s.lineHeight = ImGui::GetTextLineHeight();
	s.glyphWidth = ImGui::CalcTextSize("F").x + 1;            // We assume the font is mono-space
	s.hexCellWidth = truncf(s.glyphWidth * 2.5f);             // "FF " we include trailing space in the width to easily catch clicks everywhere
	s.spacingBetweenMidCols = truncf(s.hexCellWidth * 0.25f); // Every 'MidColsCount' columns we add a bit of extra spacing
	s.posHexStart = float(s.addrDigitsCount + 2) * s.glyphWidth;
	auto posHexEnd = s.posHexStart + (s.hexCellWidth * float(columns));
	s.posAsciiStart = s.posAsciiEnd = posHexEnd;
	if (showAscii) {
		int numMacroColumns = (columns + MidColsCount - 1) / MidColsCount;
		s.posAsciiStart = posHexEnd + s.glyphWidth + float(numMacroColumns) * s.spacingBetweenMidCols;
		s.posAsciiEnd = s.posAsciiStart + float(columns) * s.glyphWidth;
	}
	s.windowWidth = s.posAsciiEnd + style.ScrollbarSize + style.WindowPadding.x * 2 + s.glyphWidth;
	return s;
}

void DebuggableEditor::paint(MSXMotherBoard* motherBoard)
{
	if (!open || !motherBoard) return;
	auto& debugger = motherBoard->getDebugger();
	auto* debuggable = debugger.findDebuggable(getDebuggableName());
	if (!debuggable) return;

	im::ScopedFont sf(manager.fontMono);

	unsigned memSize = debuggable->getSize();
	columns = std::min(columns, narrow<int>(memSize));
	auto s = calcSizes(memSize);
	ImGui::SetNextWindowSize(ImVec2(s.windowWidth, s.windowWidth * 0.60f), ImGuiCond_FirstUseEver);

	im::Window(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar, [&]{
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
		    ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
			ImGui::OpenPopup("context");
		}
		drawContents(s, *debuggable, memSize);
	});
}

[[nodiscard]] static unsigned DataTypeGetSize(ImGuiDataType dataType)
{
	std::array<unsigned, ImGuiDataType_COUNT - 2> sizes = { 1, 1, 2, 2, 4, 4, 8, 8 };
	assert(dataType >= 0 && dataType < (ImGuiDataType_COUNT - 2));
	return sizes[dataType];
}

[[nodiscard]] static std::optional<uint8_t> parseDataValue(std::string_view str)
{
	auto parseDigit = [](char c) -> std::optional<int> {
		if ('0' <= c && c <= '9') return c - '0';
		if ('a' <= c && c <= 'f') return c - 'a' + 10;
		if ('A' <= c && c <= 'F') return c - 'A' + 10;
		return std::nullopt;
	};
	if (str.size() == 1) {
		return parseDigit(str[0]);
	} else if (str.size() == 2) {
		if (auto digit0 = parseDigit(str[0])) {
			if (auto digit1 = parseDigit(str[1])) {
				return 16 * *digit0 + *digit1;
			}
		}
	}
	return std::nullopt;
}

struct ParseAddrResult { // TODO c++23 std::expected might be a good fit here
	std::string error;
	unsigned addr = 0;
};
[[nodiscard]] static ParseAddrResult parseAddressExpr(
	std::string_view str, SymbolManager& symbolManager, Interpreter& interp)
{
	ParseAddrResult r;
	if (str.empty()) return r;

	// TODO linear search, probably OK for now, but can be improved if it turns out to be a problem
	// Note: limited to 16-bit, but larger values trigger an errors and are then handled below, so that's fine
	if (auto addr = symbolManager.parseSymbolOrValue(str)) {
		r.addr = *addr;
		return r;
	}

	try {
		r.addr = TclObject(str).eval(interp).getInt(interp);
	} catch (CommandException& e) {
		r.error = e.getMessage();
	}
	return r;
}

[[nodiscard]] static std::string formatData(uint8_t val)
{
	return strCat(hex_string<2, HexCase::upper>(val));
}

void DebuggableEditor::drawContents(const Sizes& s, Debuggable& debuggable, unsigned memSize)
{
	auto formatAddr = [&](unsigned addr) {
		return strCat(hex_string<HexCase::upper>(Digits{size_t(s.addrDigitsCount)}, addr));
	};
	auto setStrings = [&]{
		addrStr = strCat("0x", formatAddr(currentAddr));
		dataInput = formatData(debuggable.read(currentAddr));
	};
	auto setAddr = [&](unsigned addr) {
		addr = std::min(addr, memSize - 1);
		if (currentAddr == addr) return false;
		currentAddr = addr;
		setStrings();
		return true;
	};
	auto scrollAddr = [&](unsigned addr) {
		if (setAddr(addr)) {
			im::Child("##scrolling", [&]{
				int row = narrow<int>(currentAddr) / columns;
				ImGui::SetScrollFromPosY(ImGui::GetCursorStartPos().y + float(row) * ImGui::GetTextLineHeight());
			});
		}
	};

	const auto& style = ImGui::GetStyle();
	if (updateAddr) {
		updateAddr = false;
		auto addr = currentAddr;
		++currentAddr; // any change
		scrollAddr(addr);
	} else {
		// still clip addr (for the unlikely case that 'memSize' got smaller)
		setAddr(currentAddr);
	}

	float footerHeight = 0.0f;
	if (showAddress) {
		footerHeight += style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	}
	if (showDataPreview) {
		footerHeight += style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing() + 3 * ImGui::GetTextLineHeightWithSpacing();
	}
	// We begin into our scrolling region with the 'ImGuiWindowFlags_NoMove' in order to prevent click from moving the window.
	// This is used as a facility since our main click detection code doesn't assign an ActiveId so the click would normally be caught as a window-move.
	int cFlags = ImGuiWindowFlags_NoMove;
	// note: with ImGuiWindowFlags_NoNav it happens occasionally that (rapid) cursor-input is passed to the underlying MSX window
	//    without ImGuiWindowFlags_NoNav PgUp/PgDown work, but they are ALSO interpreted as openMSX hotkeys,
	//                                   though other windows have the same problem.
	//flags |= ImGuiWindowFlags_NoNav;
	cFlags |= ImGuiWindowFlags_HorizontalScrollbar;
	ImGui::BeginChild("##scrolling", ImVec2(0, -footerHeight), ImGuiChildFlags_None, cFlags);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

	std::optional<unsigned> nextAddr;
	// Move cursor but only apply on next frame so scrolling with be synchronized (because currently we can't change the scrolling while the window is being rendered)
	if (addrMode == CURSOR && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
		if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow)) &&
			int(currentAddr) >= columns) {
			nextAddr = currentAddr - columns;
		} else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow)) &&
				int(currentAddr) < int(memSize - columns)) {
			nextAddr = currentAddr + columns;
		} else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_LeftArrow)) &&
				int(currentAddr) > 0) {
			nextAddr = currentAddr - 1;
		} else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_RightArrow)) &&
				int(currentAddr) < int(memSize - 1)) {
			nextAddr = currentAddr + 1;
		}
	}

	// Draw vertical separator
	auto* drawList = ImGui::GetWindowDrawList();
	ImVec2 windowPos = ImGui::GetWindowPos();
	if (showAscii) {
		drawList->AddLine(ImVec2(windowPos.x + s.posAsciiStart - s.glyphWidth, windowPos.y),
		                  ImVec2(windowPos.x + s.posAsciiStart - s.glyphWidth, windowPos.y + 9999),
		                  ImGui::GetColorU32(ImGuiCol_Border));
	}

	const auto colorText = getColor(imColor::TEXT);
	const auto colorDisabled = greyOutZeroes ? getColor(imColor::TEXT_DISABLED) : colorText;

	// We are not really using the clipper API correctly here, because we rely on visible_start_addr/visible_end_addr for our scrolling function.
	const int totalLineCount = int((memSize + columns - 1) / columns);
	ImGuiListClipper clipper;
	clipper.Begin(totalLineCount, s.lineHeight);
	while (clipper.Step()) {
		for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
			auto addr = unsigned(line) * columns;
			ImGui::StrCat(formatAddr(addr), ':');

			// Draw Hexadecimal
			for (int n = 0; n < columns && addr < memSize; ++n, ++addr) {
				int macroColumn = n / MidColsCount;
				float bytePosX = s.posHexStart + float(n) * s.hexCellWidth
				               + float(macroColumn) * s.spacingBetweenMidCols;
				ImGui::SameLine(bytePosX);

				// Draw highlight
				auto previewDataTypeSize = DataTypeGetSize(previewDataType);
				auto highLight = [&](unsigned a) {
					return (currentAddr <= a) && (a < (currentAddr + previewDataTypeSize));
				};
				if (highLight(addr)) {
					ImVec2 pos = ImGui::GetCursorScreenPos();
					float highlightWidth = s.glyphWidth * 2;
					if (highLight(addr + 1)) {
						highlightWidth = s.hexCellWidth;
						if (n > 0 && (n + 1) < columns && ((n + 1) % MidColsCount) == 0) {
							highlightWidth += s.spacingBetweenMidCols;
						}
					}
					drawList->AddRectFilled(pos, ImVec2(pos.x + highlightWidth, pos.y + s.lineHeight), HighlightColor);
				}

				if (currentAddr == addr && (dataEditingTakeFocus || dataEditingActive)) {
					// Display text input on current byte
					if (dataEditingTakeFocus) {
						dataEditingActive = true;
						ImGui::SetKeyboardFocusHere(0);
						setStrings();
					}
					struct UserData {
						// FIXME: We should have a way to retrieve the text edit cursor position more easily in the API, this is rather tedious. This is such a ugly mess we may be better off not using InputText() at all here.
						static int Callback(ImGuiInputTextCallbackData* data) {
							auto* userData = static_cast<UserData*>(data->UserData);
							if (!data->HasSelection()) {
								userData->cursorPos = data->CursorPos;
							}
							if (data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen) {
								// When not editing a byte, always refresh its InputText content pulled from underlying memory data
								// (this is a bit tricky, since InputText technically "owns" the master copy of the buffer we edit it in there)
								uint8_t val = userData->debuggable->read(userData->addr);
								auto valStr = formatData(val);
								data->DeleteChars(0, data->BufTextLen);
								data->InsertChars(0, valStr.c_str());
								data->SelectionStart = 0;
								data->SelectionEnd = 2;
								data->CursorPos = 0;
							}
							return 0;
						}
						Debuggable* debuggable = nullptr;
						unsigned addr = 0;
						int cursorPos = -1; // Output
					};
					UserData userData;
					userData.debuggable = &debuggable;
					userData.addr = addr;
					ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsHexadecimal
					                          | ImGuiInputTextFlags_EnterReturnsTrue
					                          | ImGuiInputTextFlags_AutoSelectAll
					                          | ImGuiInputTextFlags_NoHorizontalScroll
					                          | ImGuiInputTextFlags_CallbackAlways
					                          | ImGuiInputTextFlags_AlwaysOverwrite;
					ImGui::SetNextItemWidth(s.glyphWidth * 2);
					bool dataWrite = false;
					im::ID(int(addr), [&]{
						if (ImGui::InputText("##data", &dataInput, flags, UserData::Callback, &userData)) {
							dataWrite = true;
						} else if (!dataEditingTakeFocus && !ImGui::IsItemActive()) {
							dataEditingActive = false;
						}
					});
					dataEditingTakeFocus = false;
					dataWrite |= userData.cursorPos >= 2;
					if (nextAddr) dataWrite = false;
					if (dataWrite) {
						if (auto value = parseDataValue(dataInput)) {
							debuggable.write(addr, *value);
							assert(!nextAddr);
							nextAddr = currentAddr + 1;
						}
					}
				} else {
					// NB: The trailing space is not visible but ensure there's no gap that the mouse cannot click on.
					uint8_t b = debuggable.read(addr);
					im::StyleColor(b == 0 && greyOutZeroes, ImGuiCol_Text, getColor(imColor::TEXT_DISABLED), [&]{
						ImGui::StrCat(formatData(b), ' ');
					});
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
						dataEditingTakeFocus = true;
						nextAddr = addr;
					}
				}
			}

			if (showAscii) {
				// Draw ASCII values
				ImGui::SameLine(s.posAsciiStart);
				ImVec2 pos = ImGui::GetCursorScreenPos();
				addr = unsigned(line) * columns;
				im::ID(line, [&]{
					if (ImGui::InvisibleButton("ascii", ImVec2(s.posAsciiEnd - s.posAsciiStart, s.lineHeight))) {
						nextAddr = addr + unsigned((ImGui::GetIO().MousePos.x - pos.x) / s.glyphWidth);
					}
				});
				for (int n = 0; n < columns && addr < memSize; ++n, ++addr) {
					if (addr == currentAddr) {
						drawList->AddRectFilled(pos, ImVec2(pos.x + s.glyphWidth, pos.y + s.lineHeight), ImGui::GetColorU32(HighlightColor));
					}
					uint8_t c = debuggable.read(addr);
					char display = (c < 32 || c >= 128) ? '.' : char(c);
					drawList->AddText(pos, (display == char(c)) ? colorText : colorDisabled, &display, &display + 1);
					pos.x += s.glyphWidth;
				}
			}
		}
	}
	ImGui::PopStyleVar(2);
	ImGui::EndChild();

	if (nextAddr) {
		setAddr(*nextAddr);
		dataEditingTakeFocus = true;
		addrMode = CURSOR;
	}

	if (showAddress) {
		ImGui::Separator();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Address");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(2.0f * style.FramePadding.x + ImGui::CalcTextSize("Expression").x + ImGui::GetFrameHeight());
		if (ImGui::Combo("##mode", &addrMode, "Cursor\0Expression\0")) {
			dataEditingTakeFocus = true;
		}
		ImGui::SameLine();

		std::string* as = addrMode == CURSOR ? &addrStr : &addrExpr;
		auto r = parseAddressExpr(*as, symbolManager, manager.getInterpreter());
		im::StyleColor(!r.error.empty(), ImGuiCol_Text, getColor(imColor::ERROR), [&] {
			if (addrMode == EXPRESSION && r.error.empty()) {
				scrollAddr(r.addr);
			}
			ImGui::SetNextItemWidth(15.0f * ImGui::GetFontSize());
			if (ImGui::InputText("##addr", as, ImGuiInputTextFlags_EnterReturnsTrue)) {
				auto r2 = parseAddressExpr(addrStr, symbolManager, manager.getInterpreter());
				if (r2.error.empty()) {
					scrollAddr(r2.addr);
					dataEditingTakeFocus = true;
				}
			}
			simpleToolTip([&]{
				return r.error.empty() ? strCat("0x", formatAddr(r.addr))
						: r.error;
			});
		});
		im::Font(manager.fontProp, [&]{
			HelpMarker("Address-mode:\n"
				"  Cursor: view the cursor position\n"
				"  Expression: continuously re-evaluate an expression and view that address\n"
				"\n"
				"Addresses can be entered as:\n"
				"  Decimal or hexadecimal values (e.g. 0x1234)\n"
				"  The name of a label (e.g. CHPUT)\n"
				"  A Tcl expression (e.g. [reg hl] to follow the content of register HL)\n"
				"\n"
				"Right-click to configure this view.");
		});
	}
	if (showDataPreview) {
		ImGui::Separator();
		drawPreviewLine(s, debuggable, memSize);
	}

	im::Popup("context", [&]{
		ImGui::SetNextItemWidth(7.5f * s.glyphWidth + 2.0f * style.FramePadding.x);
		if (ImGui::InputInt("Columns", &columns, 1, 0)) {
			columns = std::clamp(columns, 1, 64);
		}
		ImGui::Checkbox("Show Address bar", &showAddress);
		ImGui::Checkbox("Show Data Preview", &showDataPreview);
		ImGui::Checkbox("Show Ascii", &showAscii);
		ImGui::Checkbox("Grey out zeroes", &greyOutZeroes);
	});
}

[[nodiscard]] static const char* DataTypeGetDesc(ImGuiDataType dataType)
{
	std::array<const char*, ImGuiDataType_COUNT - 2> desc = {
		"Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64"
	};
	assert(dataType >= 0 && dataType < (ImGuiDataType_COUNT - 2));
	return desc[dataType];
}

template<typename T>
[[nodiscard]] static T read(std::span<const uint8_t> buf)
{
	assert(buf.size() >= sizeof(T));
	T t = 0;
	memcpy(&t, buf.data(), sizeof(T));
	return t;
}

static void formatDec(std::span<const uint8_t> buf, ImGuiDataType dataType)
{
	switch (dataType) {
	case ImGuiDataType_S8:
		ImGui::StrCat(read<int8_t>(buf));
		break;
	case ImGuiDataType_U8:
		ImGui::StrCat(read<uint8_t>(buf));
		break;
	case ImGuiDataType_S16:
		ImGui::StrCat(read<int16_t>(buf));
		break;
	case ImGuiDataType_U16:
		ImGui::StrCat(read<uint16_t>(buf));
		break;
	case ImGuiDataType_S32:
		ImGui::StrCat(read<int32_t>(buf));
		break;
	case ImGuiDataType_U32:
		ImGui::StrCat(read<uint32_t>(buf));
		break;
	case ImGuiDataType_S64:
		ImGui::StrCat(read<int64_t>(buf));
		break;
	case ImGuiDataType_U64:
		ImGui::StrCat(read<uint64_t>(buf));
		break;
	default:
		UNREACHABLE;
	}
}

static void formatHex(std::span<const uint8_t> buf, ImGuiDataType data_type)
{
	switch (data_type) {
	case ImGuiDataType_S8:
	case ImGuiDataType_U8:
		ImGui::StrCat(hex_string<2>(read<uint8_t>(buf)));
		break;
	case ImGuiDataType_S16:
	case ImGuiDataType_U16:
		ImGui::StrCat(hex_string<4>(read<uint16_t>(buf)));
		break;
	case ImGuiDataType_S32:
	case ImGuiDataType_U32:
		ImGui::StrCat(hex_string<8>(read<uint32_t>(buf)));
		break;
	case ImGuiDataType_S64:
	case ImGuiDataType_U64:
		ImGui::StrCat(hex_string<16>(read<uint64_t>(buf)));
		break;
	default:
		UNREACHABLE;
	}
}

static void formatBin(std::span<const uint8_t> buf)
{
	for (int i = int(buf.size()) - 1; i >= 0; --i) {
		ImGui::StrCat(bin_string<8>(buf[i]));
		if (i != 0) ImGui::SameLine();
	}
}

void DebuggableEditor::drawPreviewLine(const Sizes& s, Debuggable& debuggable, unsigned memSize)
{
	const auto& style = ImGui::GetStyle();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Preview as:"sv);
	ImGui::SameLine();
	ImGui::SetNextItemWidth((s.glyphWidth * 10.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
	if (ImGui::BeginCombo("##combo_type", DataTypeGetDesc(previewDataType), ImGuiComboFlags_HeightLargest)) {
		for (int n = 0; n < (ImGuiDataType_COUNT - 2); ++n) {
			if (ImGui::Selectable(DataTypeGetDesc((ImGuiDataType)n), previewDataType == n)) {
				previewDataType = ImGuiDataType(n);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth((s.glyphWidth * 6.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
	ImGui::Combo("##combo_endianess", &previewEndianess, "LE\0BE\0\0");

	std::array<uint8_t, 8> dataBuf = {};
	auto elemSize = DataTypeGetSize(previewDataType);
	for (auto i : xrange(elemSize)) {
		auto addr = currentAddr + i;
		dataBuf[i] = (addr < memSize) ? debuggable.read(addr) : 0;
	}

	static constexpr bool nativeIsLittle = std::endian::native == std::endian::little;
	bool previewIsLittle = previewEndianess == LE;
	if (nativeIsLittle != previewIsLittle) {
		std::reverse(dataBuf.begin(), dataBuf.begin() + elemSize);
	}

	ImGui::TextUnformatted("Dec "sv);
	ImGui::SameLine();
	formatDec(dataBuf, previewDataType);

	ImGui::TextUnformatted("Hex "sv);
	ImGui::SameLine();
	formatHex(dataBuf, previewDataType);

	ImGui::TextUnformatted("Bin "sv);
	ImGui::SameLine();
	formatBin(subspan(dataBuf, 0, elemSize));
}

} // namespace openmsx
