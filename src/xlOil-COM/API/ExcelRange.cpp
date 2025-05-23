#include <xloil/Range.h>
#include <xloil/AppObjects.h>
#include <xlOil/ExcelTypeLib.h>
#include <xlOil/ExcelRef.h>
#include <xlOil/ExcelArray.h>
#include <xlOil/AppObjects.h>
#include <xlOil/ExcelThread.h>
#include <xlOil/State.h>
#include <xlOil-COM/ComVariant.h>

namespace xloil
{
  namespace
  {
    _variant_t stringToVariant(const std::wstring_view& str)
    {
      auto variant = COM::stringToVariant(str);
      return _variant_t(variant, false);
    }
  }

  std::unique_ptr<Range> newRange(const wchar_t* address)
  {
    if (InXllContext::check())
      return std::make_unique<XllRange>(ExcelRef(address));
    else
      return std::make_unique<ExcelRange>(address);
  }
  ExcelRef refFromComRange(Excel::Range& range)
  {
    try
    {
      const auto nCols = range.Columns->Count;
      const auto nRows = range.Rows->Count;

      // Excel uses 1-based indexing for these, so we adjust
      const auto fromRow = range.Row - 1;
      const auto fromCol = range.Column - 1;

      // Convert to an XLL SheetId
      auto wb = (Excel::_WorkbookPtr)range.Worksheet->Parent;
      const auto sheetId =
        callExcel(msxll::xlSheetId, fmt::format(L"[{0}]{1}",
          wb->Name, range.Worksheet->Name));

      return ExcelRef(sheetId.val.mref.idSheet,
        fromRow, fromCol, fromRow + nRows - 1, fromCol + nCols - 1);
    }
    XLO_RETHROW_COM_ERROR;
  }

  ExcelRange::ExcelRange(const std::wstring_view& address, const Application& app)
    : AppObject([&]() {    
        try
        {
          return app.com().GetRange(stringToVariant(address)).Detach();
        }
        XLO_RETHROW_COM_ERROR; 
      }(), true)
  {
  }

  ExcelRange::ExcelRange(const Range& range)
    : AppObject(nullptr)
  {
    auto* comPtr = range.asComPtr();
    if (comPtr)
      *this = ExcelRange(comPtr);
    else
      *this = ExcelRange(range.address());
  }

  std::unique_ptr<Range> ExcelRange::range(
    int fromRow, int fromCol,
    int toRow, int toCol) const
  {
    try
    {
      if (toRow == Range::TO_END)
        toRow = com().Row + com().Rows->GetCount();
      if (toCol == Range::TO_END)
        toCol = com().Column + com().Columns->GetCount();

      // Caling range->GetRange(cell1, cell2) does a very weird thing
      // which I can't make sense of. Better to call ws.range(...)
      auto ws = (Excel::_WorksheetPtr)com().Parent;
      auto cells = com().Cells;
      auto r = ws->GetRange(
        cells->Item[fromRow + 1][fromCol + 1],
        cells->Item[toRow + 1][toCol + 1]);
      return std::make_unique<ExcelRange>(r);
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::unique_ptr<Range> ExcelRange::trim() const
  {
    // Better than SpecialCells?
    size_t nRows = 0, nCols = 0;
    if (size() == 1 || !COM::trimmedVariantArrayBounds(com().Value2, nRows, nCols))
      return std::make_unique<ExcelRange>(*this);
    // 'range' takes the last row/col inclusive so subtract one
    if (nRows > 0) --nRows;
    if (nCols > 0) --nCols;
    return range(0, 0, (int)nRows, (int)nCols);
  }

  std::tuple<Range::row_t, Range::col_t> ExcelRange::shape() const
  {
    try
    {
      return { com().Rows->GetCount(), com().Columns->GetCount() };
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::tuple<Range::row_t, Range::col_t, Range::row_t, Range::col_t> ExcelRange::bounds() const
  {
    try
    {
      const auto row = com().Row - 1;
      const auto col = com().Column - 1;
      return { row, col, row + com().Rows->GetCount() - 1, col + com().Columns->GetCount() - 1 };
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::wstring ExcelRange::address(AddressStyle style) const
  {
    try
    {
      const bool rowFixed = (style & AddressStyle::ROW_FIXED) != 0;
      const bool colFixed = (style & AddressStyle::COL_FIXED) != 0;
      const auto refStyle = (style & AddressStyle::RC) != 0
        ? Excel::xlR1C1
        : Excel::xlA1;
      const bool local = (style & AddressStyle::LOCAL) != 0;
      auto result = com().GetAddress(
        rowFixed, colFixed, refStyle, !local);
      return std::wstring(result);
    }
    XLO_RETHROW_COM_ERROR;
  }

  size_t ExcelRange::nAreas() const
  {
    try
    {
      return size_t(com().GetAreas()->Count);
    }
    XLO_RETHROW_COM_ERROR;
  }

  ExcelObj ExcelRange::value() const
  {
    return COM::variantToExcelObj(com().Value2, false, false);
  }

  ExcelObj ExcelRange::value(row_t i, col_t j) const
  {
    Excel::RangePtr range(com().Cells->Item[i + 1][j + 1]);
    return COM::variantToExcelObj(range->Value2);
  }

  void ExcelRange::set(const ExcelObj& value)
  {
    try
    {
      VARIANT v;
      COM::excelObjToVariant(&v, value);
      com().PutValue2(_variant_t(v, false)); // Move variant
    }
    XLO_RETHROW_COM_ERROR;
  }

  void ExcelRange::setFormula(const std::wstring_view& formula, const SetFormulaMode mode)
  {
    try
    {
      auto value = stringToVariant(formula);
      if (mode == ARRAY_FORMULA && size() > 1)
        com().FormulaArray = value;
      else if (mode == OLD_ARRAY)
        com().PutFormula(value);
      else
        com().PutFormula2(value);
    }
    XLO_RETHROW_COM_ERROR;
  }

  void ExcelRange::setFormula(const ExcelObj& formula, const SetFormulaMode mode)
  {
    try
    { 
      static bool dynamicArrays = Environment::excelProcess().supportsDynamicArrays;
      VARIANT v;
      COM::excelObjToVariant(&v, formula);
      auto value = _variant_t(v, false);  // Move variant
      if (mode == ARRAY_FORMULA && v.vt == VT_BSTR && size() > 1)
        com().FormulaArray = value;
      else if (mode == OLD_ARRAY || !dynamicArrays)
        com().PutFormula(value);
      else
        com().PutFormula2(value);
    }
    XLO_RETHROW_COM_ERROR;
  }

  ExcelObj ExcelRange::formula() const
  {
    try
    {
      static bool dynamicArrays = Environment::excelProcess().supportsDynamicArrays;
      return COM::variantToExcelObj(dynamicArrays ? com().Formula2 : com().Formula);
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::optional<bool> ExcelRange::hasFormula() const
  {
    try
    {
      auto result = com().HasFormula;
      if (result.vt == VT_BOOL)
        return (bool)result;
      else
        return std::optional<bool>();
    }
    XLO_RETHROW_COM_ERROR;
  }

  void ExcelRange::clear()
  {
    try
    {
      com().Clear();
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::wstring ExcelRange::name() const
  {
    return address();
  }

  ExcelWorksheet ExcelRange::parent() const
  {
    try
    {
      return ExcelWorksheet(com().Worksheet);
    }
    XLO_RETHROW_COM_ERROR;
  }

  Application ExcelRange::app() const
  {
    return parent().app();
  }
  enum class SpecialCellsValue : int
  {
    Errors = 16,
    Logical = 4,
    Numbers = 1,
    TextValues = 2,
  };

  ExcelRange ExcelRange::specialCells(SpecialCells type, ExcelType values) const
  {
    // Use raw_SpecialCells to avoid catch?
    try
    {
      _variant_t specialCellsValue = vtMissing;
      auto cellType = Excel::XlCellType(int(type));
      if (unsigned(values) != 0 && 
        (type == SpecialCells::Constants || type == SpecialCells::Formulas))
      {
        // Conveniently, the XlSpecialCellsValue enumeration matches XLL's
        // xltype enumeration, so we forward the int without modification.
        specialCellsValue = _variant_t(unsigned(values));
      }

      return ExcelRange(
        com().SpecialCells(cellType, specialCellsValue).Detach(), true);
    }
    catch (_com_error& error)
    {
      if (error.Error() == VBA_E_IGNORE)
        throw xloil::ComBusyException();
      else if (error.Error() == 0x800A03EC)
        return ExcelRange();
      else
        XLO_THROW(L"COM Error {0:#x}: {1}", (unsigned)error.Error(), error.ErrorMessage()); \
    }
  }

  ComIterator<ExcelRange> ExcelRange::begin() const
  {
    try
    {
      return ComIterator<ExcelRange>(com().Get_NewEnum());
    }
    XLO_RETHROW_COM_ERROR;
  }
}